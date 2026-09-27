/*
 * tls.c — NovaOS TLS support: BearSSL glue, root certificate store, entropy
 */

#include "tls.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../hal/rtc.h"
#include "../arch/x86_64/cpu.h"

#define USER_ROOTS_MAX  64

/* -----------------------------------------------------------------------
 * Entropy pool
 *
 * A running SHA-256 over every unpredictable input we have: RDRAND (when
 * the CPU has it), TSC readings at boot and at each received frame (the
 * net thread stirs one per packet), the RTC and the MAC address.  Each
 * connection's engine is seeded with a fresh digest of the pool plus a
 * counter; the digest is fed back so outputs never repeat.
 * ----------------------------------------------------------------------- */
static br_sha256_context g_pool;
static UINT64            g_pool_ctr;

static bool roots_init(void);

static bool rdrand64(UINT64 *out)
{
    for (int i = 0; i < 10; i++) {
        UINT64 v;
        UINT8 ok;
        /* rdrand %rax (encoded: the kernel is built without RDRAND in -march) */
        __asm__ volatile (".byte 0x48, 0x0f, 0xc7, 0xf0\n\tsetc %1"
                          : "=a"(v), "=qm"(ok) : : "cc");
        if (ok) { *out = v; return true; }
    }
    return false;
}

void TlsStirEntropy(const void *data, UINT32 len)
{
    br_sha256_update(&g_pool, data, len);
}

static void stir_tsc(void)
{
    UINT64 t = rdtsc();
    br_sha256_update(&g_pool, &t, sizeof(t));
}

void TlsInit(void)
{
    br_sha256_init(&g_pool);
    RtcTime rt;
    rtc_read(&rt);
    br_sha256_update(&g_pool, &rt, sizeof(rt));

    bool has_rdrand = (cpuid(1, 0).ecx >> 30) & 1;
    int  got = 0;
    for (int i = 0; has_rdrand && i < 8; i++) {
        UINT64 v;
        if (rdrand64(&v)) { br_sha256_update(&g_pool, &v, sizeof(v)); got++; }
    }
    /* TSC jitter around short busy loops: weak on its own, but cheap */
    for (int i = 0; i < 64; i++) {
        for (volatile int j = 0; j < (i & 7) * 50; j++) { }
        stir_tsc();
    }
    roots_init();
    kprintf("[TLS] BearSSL ready: %d trusted roots; entropy from %sTSC jitter%s\n",
            g_tls_builtin_root_count, got ? "RDRAND, " : "",
            got ? "" : " (no RDRAND: seeding also uses packet timing)");
}

static void pool_output(UINT8 out[32])
{
    br_sha256_context c = g_pool;
    UINT64 x[2] = { ++g_pool_ctr, rdtsc() };
    br_sha256_update(&c, x, sizeof(x));
    br_sha256_out(&c, out);
    br_sha256_update(&g_pool, out, 32);          /* ratchet */
}

/* -----------------------------------------------------------------------
 * Root store
 * ----------------------------------------------------------------------- */
static br_x509_trust_anchor *g_roots;            /* built-in, then user */
static int                   g_nroots;

static bool roots_init(void)
{
    if (g_roots) return true;
    g_roots = kmalloc(sizeof(*g_roots) * (size_t)(g_tls_builtin_root_count + USER_ROOTS_MAX));
    if (!g_roots) return false;
    memcpy(g_roots, g_tls_builtin_roots, sizeof(*g_roots) * (size_t)g_tls_builtin_root_count);
    g_nroots = g_tls_builtin_root_count;
    return true;
}

int TlsRootCount(void)     { return g_roots ? g_nroots : g_tls_builtin_root_count; }
int TlsUserRootCount(void) { return g_roots ? g_nroots - g_tls_builtin_root_count : 0; }

/* Read one DER TLV header; returns false if malformed. */
static bool der_next(const UINT8 **p, const UINT8 *end, UINT8 *tag,
                     const UINT8 **val, size_t *vlen)
{
    const UINT8 *q = *p;
    if (end - q < 2) return false;
    *tag = *q++;
    size_t len = *q++;
    if (len & 0x80) {
        int n = (int)(len & 0x7F);
        if (n == 0 || n > 3 || end - q < n) return false;
        len = 0;
        while (n--) len = (len << 8) | *q++;
    }
    if ((size_t)(end - q) < len) return false;
    *val = q; *vlen = len;
    *p = q + len;
    return true;
}

/* Copy an X.500 string value as printable ASCII (BMPString: low bytes). */
static void copy_str(UINT8 tag, const UINT8 *v, size_t n, char *buf, int cap)
{
    int o = 0;
    size_t step = (tag == 0x1E) ? 2 : 1;          /* BMPString is UTF-16BE */
    for (size_t i = (step == 2 ? 1 : 0); i < n && o < cap - 1; i += step) {
        UINT8 c = v[i];
        if (step == 2 && v[i - 1]) c = '?';
        buf[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : (c >= 0x80 ? '?' : ' ');
    }
    buf[o] = '\0';
}

bool TlsRootName(int i, char *buf, int cap, bool *user)
{
    if (!roots_init() || i < 0 || i >= g_nroots || cap < 2) return false;
    if (user) *user = i >= g_tls_builtin_root_count;
    const UINT8 *p = g_roots[i].dn.data, *end = p + g_roots[i].dn.len;
    UINT8 tag; const UINT8 *v; size_t n;
    buf[0] = '\0';
    if (!der_next(&p, end, &tag, &v, &n) || tag != 0x30) return true;
    p = v; end = v + n;
    static const UINT8 prefer[3] = { 0x03, 0x0A, 0x0B };     /* CN, O, OU */
    int best = 3;
    while (p < end) {                                          /* SET */
        const UINT8 *sv; size_t sn;
        if (!der_next(&p, end, &tag, &sv, &sn) || tag != 0x31) break;
        const UINT8 *q = sv, *qe = sv + sn;
        while (q < qe) {                                       /* SEQUENCE */
            const UINT8 *av; size_t an;
            if (!der_next(&q, qe, &tag, &av, &an) || tag != 0x30) break;
            const UINT8 *r = av, *re = av + an, *oid, *val; size_t oidn, valn;
            UINT8 vt;
            if (!der_next(&r, re, &tag, &oid, &oidn) || tag != 0x06) break;
            if (!der_next(&r, re, &vt, &val, &valn)) break;
            if (oidn == 3 && oid[0] == 0x55 && oid[1] == 0x04)
                for (int k = 0; k < best; k++)
                    if (oid[2] == prefer[k]) { copy_str(vt, val, valn, buf, cap); best = k; break; }
        }
    }
    return true;
}

typedef struct { UINT8 *data; size_t len, cap; } Buf;

static void buf_append(void *ctx, const void *data, size_t len)
{
    Buf *b = ctx;
    if (!b->data) return;                                      /* earlier OOM */
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + len) cap *= 2;
        UINT8 *nd = kmalloc(cap);
        if (!nd) { kfree(b->data); b->data = NULL; return; }
        memcpy(nd, b->data, b->len);
        kfree(b->data);
        b->data = nd; b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void *dup_bytes(const void *p, size_t n)
{
    void *d = kmalloc(n ? n : 1);
    if (d) memcpy(d, p, n);
    return d;
}

/* Add one DER certificate as a trust anchor.  0 = added, 1 = duplicate,
 * -1 = error (err filled). */
static int add_der(const UINT8 *der, size_t len, char *err, int cap)
{
    if (g_nroots >= g_tls_builtin_root_count + USER_ROOTS_MAX) {
        ksnprintf(err, cap, "The certificate store is full"); return -1;
    }
    Buf dn = { kmalloc(256), 0, 256 };
    if (!dn.data) { ksnprintf(err, cap, "Out of memory"); return -1; }
    br_x509_decoder_context *dc = kmalloc(sizeof(*dc));
    if (!dc) { kfree(dn.data); ksnprintf(err, cap, "Out of memory"); return -1; }
    br_x509_decoder_init(dc, buf_append, &dn);
    br_x509_decoder_push(dc, der, len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(dc);
    if (!pk || !dn.data) {
        ksnprintf(err, cap, "Not a valid X.509 certificate (error %d)",
                  br_x509_decoder_last_error(dc));
        kfree(dc); kfree(dn.data); return -1;
    }
    if (!br_x509_decoder_isCA(dc)) {
        ksnprintf(err, cap, "Not a CA certificate (basicConstraints CA:FALSE)");
        kfree(dc); kfree(dn.data); return -1;
    }

    br_x509_trust_anchor ta;
    memset(&ta, 0, sizeof(ta));
    ta.flags = BR_X509_TA_CA;
    ta.pkey.key_type = pk->key_type;
    bool ok = true;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta.pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen);
        ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta.pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen);
        ta.pkey.key.rsa.elen = pk->key.rsa.elen;
        ok = ta.pkey.key.rsa.n && ta.pkey.key.rsa.e;
    } else {
        ta.pkey.key.ec.curve = pk->key.ec.curve;
        ta.pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen);
        ta.pkey.key.ec.qlen = pk->key.ec.qlen;
        ok = ta.pkey.key.ec.q != NULL;
    }
    kfree(dc);
    ta.dn.data = dn.data;
    ta.dn.len  = dn.len;
    if (!ok) { ksnprintf(err, cap, "Out of memory"); return -1; }   /* (small leak) */

    for (int i = 0; i < g_nroots; i++) {                        /* already trusted? */
        const br_x509_trust_anchor *t = &g_roots[i];
        if (t->dn.len != ta.dn.len || memcmp(t->dn.data, ta.dn.data, ta.dn.len)) continue;
        if (t->pkey.key_type != ta.pkey.key_type) continue;
        bool same = ta.pkey.key_type == BR_KEYTYPE_RSA
            ? t->pkey.key.rsa.nlen == ta.pkey.key.rsa.nlen &&
              !memcmp(t->pkey.key.rsa.n, ta.pkey.key.rsa.n, ta.pkey.key.rsa.nlen)
            : t->pkey.key.ec.qlen == ta.pkey.key.ec.qlen &&
              !memcmp(t->pkey.key.ec.q, ta.pkey.key.ec.q, ta.pkey.key.ec.qlen);
        if (same) {
            kfree(ta.dn.data);
            if (ta.pkey.key_type == BR_KEYTYPE_RSA) { kfree(ta.pkey.key.rsa.n); kfree(ta.pkey.key.rsa.e); }
            else kfree(ta.pkey.key.ec.q);
            return 1;
        }
    }
    /* Appending never moves existing entries, so connections that were
     * handed (g_roots, old count) keep a valid view. */
    g_roots[g_nroots++] = ta;
    return 0;
}

int TlsImportRoots(const void *data, UINT32 len, char *err, int err_cap)
{
    err[0] = '\0';
    if (!roots_init()) { ksnprintf(err, err_cap, "Out of memory"); return 0; }
    const UINT8 *d = data;
    int added = 0, dups = 0, certs = 0;

    bool pem = false;
    for (UINT32 i = 0; i + 10 <= len; i++)
        if (!memcmp(d + i, "-----BEGIN", 10)) { pem = true; break; }

    if (!pem) {                                   /* a single DER certificate */
        int r = add_der(d, len, err, err_cap);
        if (r == 0) return 1;
        if (r == 1) ksnprintf(err, err_cap, "Certificate is already trusted");
        return 0;
    }

    br_pem_decoder_context *pc = kmalloc(sizeof(*pc));
    if (!pc) { ksnprintf(err, err_cap, "Out of memory"); return 0; }
    br_pem_decoder_init(pc);
    Buf obj = { NULL, 0, 0 };
    bool in_cert = false;
    UINT32 off = 0;
    bool   tail = false;                  /* a final "\n" ends a last line */
    while (off < len || !tail) {
        if (off < len) off += (UINT32)br_pem_decoder_push(pc, d + off, len - off);
        else           { br_pem_decoder_push(pc, "\n", 1); tail = true; }
        int ev = br_pem_decoder_event(pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            in_cert = !strcmp(br_pem_decoder_name(pc), "CERTIFICATE") ||
                      !strcmp(br_pem_decoder_name(pc), "X509 CERTIFICATE");
            kfree(obj.data);
            obj.data = in_cert ? kmalloc(2048) : NULL;
            obj.len = 0; obj.cap = obj.data ? 2048 : 0;
            br_pem_decoder_setdest(pc, in_cert ? buf_append : NULL, &obj);
        } else if (ev == BR_PEM_END_OBJ) {
            if (in_cert && obj.data) {
                certs++;
                int r = add_der(obj.data, obj.len, err, err_cap);
                if (r == 0) added++; else if (r == 1) dups++;
            }
            in_cert = false;
        } else if (ev == BR_PEM_ERROR) {
            ksnprintf(err, err_cap, "Malformed PEM data");
            break;
        }
    }
    kfree(obj.data);
    kfree(pc);
    if (!added && !err[0])
        ksnprintf(err, err_cap, certs ? (dups ? "Certificate is already trusted"
                                              : "No usable certificates")
                                      : "No certificates found");
    return added;
}

/* -----------------------------------------------------------------------
 * Client connections
 * ----------------------------------------------------------------------- */

/* Days since 1 January 0 AD (proleptic Gregorian), as BearSSL expects. */
static UINT32 days_since_0ad(int y, int m, int d)
{
    /* days_from_civil (H. Hinnant), relative to 1970-01-01 */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days1970 = (long)era * 146097 + doe - 719468;
    return (UINT32)(days1970 + 719528);
}

TlsConn *TlsClientNew(const char *host)
{
    if (!roots_init()) return NULL;
    TlsConn *c = kmalloc(sizeof(*c));
    if (!c) return NULL;
    br_ssl_client_init_full(&c->cc, &c->xc, g_roots, (size_t)g_nroots);

    RtcTime t;                                    /* RTC runs on UTC */
    rtc_read(&t);
    br_x509_minimal_set_time(&c->xc, days_since_0ad(t.year, t.month, t.day),
                             (UINT32)t.hour * 3600 + t.minute * 60 + t.second);

    br_ssl_engine_set_buffer(&c->cc.eng, c->iobuf, sizeof(c->iobuf), 1);
    UINT8 seed[32];
    pool_output(seed);
    br_ssl_engine_inject_entropy(&c->cc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    if (!br_ssl_client_reset(&c->cc, host, 0)) {
        kfree(c);
        return NULL;
    }
    return c;
}

void TlsFree(TlsConn *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));                     /* wipe keys */
    kfree(c);
}

const char *TlsErrorText(int err, char *buf, int cap)
{
    const char *s = NULL;
    switch (err) {
    case BR_ERR_X509_NOT_TRUSTED:
        s = "Certificate is not trusted (unknown certificate authority)"; break;
    case BR_ERR_X509_EXPIRED:
        s = "Certificate has expired or is not yet valid"; break;
    case BR_ERR_X509_BAD_SERVER_NAME:
        s = "Certificate does not match the host name"; break;
    case BR_ERR_X509_BAD_SIGNATURE:
        s = "Certificate signature is invalid"; break;
    case BR_ERR_X509_NOT_CA:
    case BR_ERR_X509_DN_MISMATCH:
        s = "Certificate chain is invalid"; break;
    case BR_ERR_X509_WEAK_PUBLIC_KEY:
        s = "Certificate key is too weak"; break;
    case BR_ERR_X509_UNSUPPORTED:
    case BR_ERR_X509_WRONG_KEY_TYPE:
        s = "Certificate uses an unsupported algorithm"; break;
    case BR_ERR_X509_TIME_UNKNOWN:
        s = "Clock is not set; cannot check certificate dates"; break;
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
        s = "Server requires an unsupported TLS version"; break;
    case BR_ERR_BAD_CIPHER_SUITE:
        s = "No cipher suite in common with the server"; break;
    case BR_ERR_BAD_MAC:
        s = "Data was corrupted in transit (bad MAC)"; break;
    case BR_ERR_NO_RANDOM:
        s = "No entropy available"; break;
    }
    if (s) return s;
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT) {
        int a = err - BR_ERR_RECV_FATAL_ALERT;
        ksnprintf(buf, cap, "Server refused the connection (TLS alert %d%s)", a,
                  a == 40 ? ": handshake failure" : a == 70 ? ": protocol version" :
                  a == 112 ? ": unrecognized name" : "");
    } else if (err >= BR_ERR_SEND_FATAL_ALERT) {
        ksnprintf(buf, cap, "TLS handshake failed (error %d)", err - BR_ERR_SEND_FATAL_ALERT);
    } else if (err >= BR_ERR_X509_OK) {
        ksnprintf(buf, cap, "Certificate is invalid (X.509 error %d)", err);
    } else {
        ksnprintf(buf, cap, "TLS error %d", err);
    }
    return buf;
}
