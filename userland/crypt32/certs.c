/*
 * certs.c — certificates, stores and chains (crypt32), on Mbed TLS.
 *
 * A certificate context is the DER encoding with its CERT_INFO filled in
 * from Mbed TLS's parse.  Stores are lists of contexts: memory stores, and
 * the system stores by name, of which "ROOT" holds the trusted roots
 * Schannel and NetSurf use (the Mozilla list in
 * C:\Windows\System32\ca-bundle.der plus certutil's CertStore) and the
 * others start empty.  CertGetCertificateChain builds the chain from the
 * certificate, the caller's extra store and the roots, and checks it with
 * Mbed TLS (signatures, validity times, the requested extended key
 * usages); it reports what failed as Windows' CERT_TRUST_* bits.  No
 * revocation lists are fetched: a chain whose revocation was asked to be
 * checked says so (CERT_TRUST_REVOCATION_STATUS_UNKNOWN, offline), as
 * Windows does when it cannot reach them.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MBEDTLS_ALLOW_PRIVATE_ACCESS              /* (a certificate's raw parts) */
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/sha1.h"
#include "tls_glue.h"

#define CRYPT32API __declspec(dllexport)
#define CRYPT_E_NOT_FOUND_    0x80092004L
#define CRYPT_E_EXISTS_       0x80092005L
#define CRYPT_E_ASN1_BADTAG_  0x8009310BL

#define X509_ASN_ENCODING_    0x1
#define CERT_STORE_PROV_MEMORY_      2
#define CERT_STORE_PROV_SYSTEM_A_    9
#define CERT_STORE_PROV_SYSTEM_W_    10
#define CERT_STORE_PROV_COLLECTION_  11
#define CERT_STORE_PROV_SYSTEM_REGISTRY_W_ 13

#define CERT_STORE_ADD_NEW_               1
#define CERT_STORE_ADD_USE_EXISTING_      2
#define CERT_STORE_ADD_REPLACE_EXISTING_  3
#define CERT_STORE_ADD_ALWAYS_            4

/* CERT_TRUST_* */
#define TRUST_NOT_TIME_VALID          0x00000001
#define TRUST_IS_REVOKED              0x00000004
#define TRUST_NOT_SIGNATURE_VALID     0x00000008
#define TRUST_NOT_VALID_FOR_USAGE     0x00000010
#define TRUST_IS_UNTRUSTED_ROOT       0x00000020
#define TRUST_REVOCATION_UNKNOWN      0x00000040
#define TRUST_INVALID_EXTENSION       0x00000100
#define TRUST_IS_PARTIAL_CHAIN        0x00010000
#define TRUST_IS_OFFLINE_REVOCATION   0x01000000
#define TRUST_HAS_EXACT_MATCH_ISSUER  0x00000001
#define TRUST_HAS_NAME_MATCH_ISSUER   0x00000004
#define TRUST_IS_SELF_SIGNED          0x00000008
#define TRUST_HAS_PREFERRED_ISSUER    0x00000100

typedef struct { DWORD cbData; BYTE *pbData; } BLOB_;
typedef struct { DWORD cbData; BYTE *pbData; DWORD cUnusedBits; } BIT_BLOB_;
typedef struct { LPSTR pszObjId; BLOB_ Parameters; } ALG_ID_;
typedef struct { ALG_ID_ Algorithm; BIT_BLOB_ PublicKey; } PUBKEY_INFO_;
typedef struct {
    DWORD dwVersion;
    BLOB_ SerialNumber;
    ALG_ID_ SignatureAlgorithm;
    BLOB_ Issuer;
    FILETIME NotBefore, NotAfter;
    BLOB_ Subject;
    PUBKEY_INFO_ SubjectPublicKeyInfo;
    BIT_BLOB_ IssuerUniqueId, SubjectUniqueId;
    DWORD cExtension;
    void *rgExtension;
} CERT_INFO_;
typedef struct { DWORD dwCertEncodingType; BYTE *pbCertEncoded; DWORD cbCertEncoded; CERT_INFO_ *pCertInfo; HANDLE hCertStore; } CERT_CONTEXT_;

typedef struct Cert {
    CERT_CONTEXT_ ctx;                  /* (first: the context the caller holds is the Cert) */
    CERT_INFO_    info;
    LONG          refs;
    char          sig_oid[64], key_oid[64];
    BYTE         *der;
    BYTE         *serial;               /* little-endian, as Windows keeps it */
} Cert;

typedef struct Store {
    DWORD   magic;
    LONG    refs;
    Cert  **certs;
    DWORD   n, cap;
    struct Store *sib[8];               /* a collection's stores */
    DWORD   nsib;
    BOOL    collection;
} Store;
#define STORE_MAGIC 0x53544F52          /* "STOR" */

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_lock); return TRUE; }
static void lock(void)   { InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL); EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* Mbed TLS starts once, under the lock: threads may race to the first call */
static const mbedtls_x509_crt *roots(void)
{
    lock();
    int bad = nova_tls_init("C:\\Windows\\System32\\ca-bundle.der");
    unlock();
    return bad ? NULL : nova_tls_roots();
}

/* -----------------------------------------------------------------------
 * Contexts
 * ----------------------------------------------------------------------- */
static void filetime_of(const mbedtls_x509_time *t, FILETIME *ft)
{
    SYSTEMTIME st = { (WORD)t->year, (WORD)t->mon, 0, (WORD)t->day, (WORD)t->hour, (WORD)t->min, (WORD)t->sec, 0 };
    if (!SystemTimeToFileTime(&st, ft)) ft->dwLowDateTime = ft->dwHighDateTime = 0;
}

static void oid_text(const mbedtls_x509_buf *oid, char *out, size_t cap)
{
    if (mbedtls_oid_get_numeric_string(out, cap, oid) < 0) out[0] = 0;
}

static Cert *cert_new(const BYTE *der, DWORD n)
{
    /* Mbed TLS keeps parsed keys in PSA, which must be up before any parse */
    roots();
    mbedtls_x509_crt x;
    mbedtls_x509_crt_init(&x);
    if (!der || !n || mbedtls_x509_crt_parse_der(&x, der, n)) {
        mbedtls_x509_crt_free(&x);
        SetLastError(CRYPT_E_ASN1_BADTAG_);
        return NULL;
    }
    Cert *c = calloc(1, sizeof(*c));
    BYTE *copy = malloc(x.raw.len), *serial = malloc(x.serial.len ? x.serial.len : 1);
    if (!c || !copy || !serial) {
        free(c); free(copy); free(serial);
        mbedtls_x509_crt_free(&x);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(copy, x.raw.p, x.raw.len);
    for (size_t i = 0; i < x.serial.len; i++) serial[i] = x.serial.p[x.serial.len - 1 - i];
    c->der = copy;
    c->serial = serial;
    c->refs = 1;
    c->ctx.dwCertEncodingType = X509_ASN_ENCODING_;
    c->ctx.pbCertEncoded = copy;
    c->ctx.cbCertEncoded = (DWORD)x.raw.len;
    c->ctx.pCertInfo = &c->info;
    /* The info's blobs point into the copy of the encoding */
    #define IN_COPY(buf) (copy + ((buf).p - x.raw.p))
    c->info.dwVersion = (DWORD)(x.version - 1);
    c->info.SerialNumber.cbData = (DWORD)x.serial.len;
    c->info.SerialNumber.pbData = serial;
    oid_text(&x.sig_oid, c->sig_oid, sizeof(c->sig_oid));
    c->info.SignatureAlgorithm.pszObjId = c->sig_oid;
    c->info.Issuer.cbData = (DWORD)x.issuer_raw.len;
    c->info.Issuer.pbData = IN_COPY(x.issuer_raw);
    c->info.Subject.cbData = (DWORD)x.subject_raw.len;
    c->info.Subject.pbData = IN_COPY(x.subject_raw);
    filetime_of(&x.valid_from, &c->info.NotBefore);
    filetime_of(&x.valid_to, &c->info.NotAfter);
    /* SubjectPublicKeyInfo: SEQUENCE { SEQUENCE { OID, params }, BIT STRING } */
    const BYTE *p = IN_COPY(x.pk_raw), *end = p + x.pk_raw.len;
    size_t len;
    unsigned char *q = (unsigned char *)p;
    if (!mbedtls_asn1_get_tag(&q, end, &len, 0x30) &&
        !mbedtls_asn1_get_tag(&q, end, &len, 0x30)) {
        unsigned char *alg_end = q + len;
        mbedtls_x509_buf oid = { 0x06, 0, NULL };
        if (!mbedtls_asn1_get_tag(&q, alg_end, &oid.len, 0x06)) {
            oid.p = q;
            oid_text(&oid, c->key_oid, sizeof(c->key_oid));
            q += oid.len;
            if (q < alg_end) { c->info.SubjectPublicKeyInfo.Algorithm.Parameters.cbData = (DWORD)(alg_end - q);
                               c->info.SubjectPublicKeyInfo.Algorithm.Parameters.pbData = q; }
        }
        q = alg_end;
        if (!mbedtls_asn1_get_tag(&q, end, &len, 0x03) && len) {
            c->info.SubjectPublicKeyInfo.PublicKey.cUnusedBits = q[0];
            c->info.SubjectPublicKeyInfo.PublicKey.cbData = (DWORD)len - 1;
            c->info.SubjectPublicKeyInfo.PublicKey.pbData = q + 1;
        }
    }
    c->info.SubjectPublicKeyInfo.Algorithm.pszObjId = c->key_oid;
    #undef IN_COPY
    mbedtls_x509_crt_free(&x);
    return c;
}

static Cert *cert_ref(Cert *c) { if (c) InterlockedIncrement(&c->refs); return c; }

static void cert_unref(Cert *c)
{
    if (!c || InterlockedDecrement(&c->refs)) return;
    free(c->der);
    free(c->serial);
    free(c);
}

CRYPT32API const void *WINAPI CertCreateCertificateContext(DWORD enc, const BYTE *b, DWORD n)
{
    (void)enc;
    return cert_new(b, n);
}
CRYPT32API const void *WINAPI CertDuplicateCertificateContext(const void *c) { return cert_ref((Cert *)c); }
CRYPT32API BOOL WINAPI CertFreeCertificateContext(const void *c) { cert_unref((Cert *)c); return TRUE; }

CRYPT32API BOOL WINAPI CertCompareCertificate(DWORD enc, CERT_INFO_ *a, CERT_INFO_ *b)
{
    (void)enc;
    return a && b && a->Issuer.cbData == b->Issuer.cbData && !memcmp(a->Issuer.pbData, b->Issuer.pbData, a->Issuer.cbData) &&
           a->SerialNumber.cbData == b->SerialNumber.cbData &&
           !memcmp(a->SerialNumber.pbData, b->SerialNumber.pbData, a->SerialNumber.cbData);
}

/* CERT_HASH_PROP_ID / CERT_SHA1_HASH_PROP_ID: the SHA-1 of the encoding */
CRYPT32API BOOL WINAPI CertGetCertificateContextProperty(const void *cv, DWORD id, void *data, DWORD *n)
{
    const Cert *c = cv;
    if (!c || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (id != 3 /* CERT_SHA1_HASH_PROP_ID */) { SetLastError(CRYPT_E_NOT_FOUND_); return FALSE; }
    if (!data) { *n = 20; return TRUE; }
    if (*n < 20) { *n = 20; SetLastError(ERROR_MORE_DATA); return FALSE; }
    mbedtls_sha1(c->der, c->ctx.cbCertEncoded, data);
    *n = 20;
    return TRUE;
}

CRYPT32API BOOL WINAPI CertGetEnhancedKeyUsage(const void *c, DWORD flags, void *usage, DWORD *n)
{
    (void)c; (void)flags; (void)usage; (void)n;
    SetLastError(CRYPT_E_NOT_FOUND_);                   /* (good for every usage) */
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Stores
 * ----------------------------------------------------------------------- */
static Store *store_of(HANDLE h)
{
    Store *s = h;
    if (!s || s->magic != STORE_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return s;
}

static Store *store_new(void)
{
    Store *s = calloc(1, sizeof(*s));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    s->magic = STORE_MAGIC;
    s->refs = 1;
    return s;
}

static BOOL store_put(Store *s, Cert *c)
{
    if (s->n == s->cap) {
        DWORD cap = s->cap ? 2 * s->cap : 16;
        Cert **p = realloc(s->certs, cap * sizeof(*p));
        if (!p) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        s->certs = p;
        s->cap = cap;
    }
    c->ctx.hCertStore = s;
    s->certs[s->n++] = cert_ref(c);
    return TRUE;
}

/* The ROOT store: the trusted roots, each as a context */
static void fill_roots(Store *s)
{
    lock();
    for (const mbedtls_x509_crt *r = roots(); r && r->raw.p; r = r->next) {
        Cert *c = cert_new(r->raw.p, (DWORD)r->raw.len);
        if (c) { store_put(s, c); cert_unref(c); }
    }
    unlock();
}

static BOOL name_is_root(const WCHAR *w, const char *a)
{
    char n[8] = { 0 };
    for (int i = 0; i < 6; i++) {
        int ch = w ? w[i] : a ? (BYTE)a[i] : 0;
        if (!ch) break;
        n[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    }
    return !strcmp(n, "ROOT") || !strcmp(n, "AUTHRO");     /* (AuthRoot: the third-party roots) */
}

static HANDLE open_store(BOOL root)
{
    Store *s = store_new();
    if (s && root) fill_roots(s);
    return s;
}

CRYPT32API HANDLE WINAPI CertOpenStore(LPCSTR provider, DWORD enc, ULONG_PTR prov, DWORD flags, const void *para)
{
    (void)enc; (void)prov; (void)flags;
    ULONG_PTR kind = (ULONG_PTR)provider;
    if (kind > 0xFFFF) return open_store(FALSE);        /* (a provider by name: an empty store) */
    if (kind == CERT_STORE_PROV_SYSTEM_W_ || kind == CERT_STORE_PROV_SYSTEM_REGISTRY_W_)
        return open_store(para && name_is_root(para, NULL));
    if (kind == CERT_STORE_PROV_SYSTEM_A_) return open_store(para && name_is_root(NULL, para));
    Store *s = open_store(FALSE);
    if (s && kind == CERT_STORE_PROV_COLLECTION_) s->collection = TRUE;
    return s;
}
CRYPT32API HANDLE WINAPI CertOpenSystemStoreW(ULONG_PTR prov, LPCWSTR name) { (void)prov; return open_store(name && name_is_root(name, NULL)); }
CRYPT32API HANDLE WINAPI CertOpenSystemStoreA(ULONG_PTR prov, LPCSTR name)  { (void)prov; return open_store(name && name_is_root(NULL, name)); }

CRYPT32API HANDLE WINAPI CertDuplicateStore(HANDLE h)
{
    Store *s = store_of(h);
    if (s) InterlockedIncrement(&s->refs);
    return h;
}

CRYPT32API BOOL WINAPI CertCloseStore(HANDLE h, DWORD flags)
{
    (void)flags;
    if (!h) return TRUE;
    Store *s = store_of(h);
    if (!s) return FALSE;
    if (InterlockedDecrement(&s->refs)) return TRUE;
    for (DWORD i = 0; i < s->n; i++) cert_unref(s->certs[i]);
    for (DWORD i = 0; i < s->nsib; i++) CertCloseStore(s->sib[i], 0);
    free(s->certs);
    s->magic = 0;
    free(s);
    return TRUE;
}

CRYPT32API BOOL WINAPI CertAddStoreToCollection(HANDLE coll, HANDLE sib, DWORD flags, DWORD prio)
{
    (void)flags; (void)prio;
    Store *c = store_of(coll), *s = store_of(sib);
    if (!c || !s) return FALSE;
    lock();
    BOOL ok = c->nsib < sizeof(c->sib) / sizeof(c->sib[0]);
    if (ok) c->sib[c->nsib++] = CertDuplicateStore(s);
    unlock();
    if (!ok) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return ok;
}

/* The certificate after @prev in @s and its collection members, or NULL */
static Cert *next_in(Store *s, const Cert *prev)
{
    BOOL seen = prev == NULL;
    Store *all[9] = { s };
    DWORD na = 1;
    for (DWORD i = 0; i < s->nsib; i++) all[na++] = s->sib[i];
    for (DWORD k = 0; k < na; k++)
        for (DWORD i = 0; i < all[k]->n; i++) {
            if (seen) return all[k]->certs[i];
            if (all[k]->certs[i] == prev) seen = TRUE;
        }
    return NULL;
}

static Cert *find_same(Store *s, const Cert *c)
{
    for (DWORD i = 0; i < s->n; i++)
        if (s->certs[i]->ctx.cbCertEncoded == c->ctx.cbCertEncoded && !memcmp(s->certs[i]->der, c->der, c->ctx.cbCertEncoded))
            return s->certs[i];
    return NULL;
}

CRYPT32API BOOL WINAPI CertAddCertificateContextToStore(HANDLE h, const void *cv, DWORD disp, const void **out)
{
    Store *s = store_of(h);
    Cert *c = (Cert *)cv;
    if (out) *out = NULL;
    if (!s) return FALSE;
    if (!c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    lock();
    Cert *have = disp == CERT_STORE_ADD_ALWAYS_ ? NULL : find_same(s, c);
    BOOL ok = TRUE;
    if (have && disp == CERT_STORE_ADD_NEW_) { ok = FALSE; SetLastError(CRYPT_E_EXISTS_); }
    else if (have) { if (out) *out = cert_ref(have); }
    else {
        Cert *copy = cert_new(c->der, c->ctx.cbCertEncoded);   /* the store's own context */
        ok = copy && store_put(s, copy);
        if (ok && out) *out = cert_ref(copy);
        cert_unref(copy);
    }
    unlock();
    return ok;
}

CRYPT32API BOOL WINAPI CertAddEncodedCertificateToStore(HANDLE h, DWORD enc, const BYTE *data, DWORD n, DWORD disp, const void **out)
{
    Cert *c = cert_new(data, n);
    if (out) *out = NULL;
    if (!c) return FALSE;
    (void)enc;
    BOOL ok = CertAddCertificateContextToStore(h, c, disp, out);
    cert_unref(c);
    return ok;
}

CRYPT32API BOOL WINAPI CertDeleteCertificateFromStore(const void *cv)
{
    Cert *c = (Cert *)cv;
    if (!c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Store *s = c->ctx.hCertStore;
    lock();
    if (s && s->magic == STORE_MAGIC)
        for (DWORD i = 0; i < s->n; i++)
            if (s->certs[i] == c) {
                memmove(s->certs + i, s->certs + i + 1, (s->n - i - 1) * sizeof(*s->certs));
                s->n--;
                cert_unref(c);                          /* the store's reference */
                break;
            }
    unlock();
    cert_unref(c);                                      /* the caller's */
    return TRUE;
}

CRYPT32API const void *WINAPI CertEnumCertificatesInStore(HANDLE h, const void *prev)
{
    Store *s = store_of(h);
    if (!s) return NULL;
    lock();
    Cert *c = cert_ref(next_in(s, prev));
    unlock();
    cert_unref((Cert *)prev);
    if (!c) SetLastError(CRYPT_E_NOT_FOUND_);
    return c;
}

/* CERT_FIND_ANY, CERT_FIND_EXISTING, CERT_FIND_SUBJECT_NAME and
 * CERT_FIND_ISSUER_NAME (encoded names), CERT_FIND_SHA1_HASH */
CRYPT32API const void *WINAPI CertFindCertificateInStore(HANDLE h, DWORD enc, DWORD flags, DWORD type, const void *para, const void *prev)
{
    (void)enc; (void)flags;
    Store *s = store_of(h);
    if (!s) return NULL;
    DWORD cmp = type >> 16;
    lock();
    Cert *c = (Cert *)prev;
    for (;;) {
        c = next_in(s, c);
        if (!c) break;
        BOOL hit;
        if (type == 0) hit = TRUE;                                              /* ANY */
        else if (cmp == 13) hit = para && find_same(&(Store){ .certs = &c, .n = 1 }, para) != NULL;  /* EXISTING */
        else if (cmp == 7 || cmp == 4) {                                        /* SUBJECT_NAME, ISSUER_NAME */
            const BLOB_ *b = para, *mine = cmp == 7 ? &c->info.Subject : &c->info.Issuer;
            hit = b && b->cbData == mine->cbData && !memcmp(b->pbData, mine->pbData, b->cbData);
        } else if (cmp == 1) {                                                  /* SHA1_HASH */
            const BLOB_ *b = para;
            BYTE d[20];
            mbedtls_sha1(c->der, c->ctx.cbCertEncoded, d);
            hit = b && b->cbData == 20 && !memcmp(b->pbData, d, 20);
        } else hit = FALSE;
        if (hit) break;
    }
    cert_ref(c);
    unlock();
    cert_unref((Cert *)prev);
    if (!c) SetLastError(CRYPT_E_NOT_FOUND_);
    return c;
}

/* -----------------------------------------------------------------------
 * Chains
 * ----------------------------------------------------------------------- */
typedef struct { DWORD dwErrorStatus, dwInfoStatus; } TRUST_STATUS_;
typedef struct {
    DWORD cbSize;
    const CERT_CONTEXT_ *pCertContext;
    TRUST_STATUS_ TrustStatus;
    void *pRevocationInfo, *pIssuanceUsage, *pApplicationUsage;
    LPCWSTR pwszExtendedErrorInfo;
} CHAIN_ELEMENT_;
typedef struct {
    DWORD cbSize;
    TRUST_STATUS_ TrustStatus;
    DWORD cElement;
    CHAIN_ELEMENT_ **rgpElement;
    void *pTrustListInfo;
    BOOL fHasRevocationFreshnessTime;
    DWORD dwRevocationFreshnessTime;
} SIMPLE_CHAIN_;
typedef struct {
    DWORD cbSize;
    TRUST_STATUS_ TrustStatus;
    DWORD cChain;
    SIMPLE_CHAIN_ **rgpChain;
    DWORD cLowerQualityChainContext;
    void **rgpLowerQualityChainContext;
    BOOL fHasRevocationFreshnessTime;
    DWORD dwRevocationFreshnessTime;
    DWORD dwCreateFlags;
    GUID ChainId;
} CHAIN_CONTEXT_;
typedef struct { DWORD dwType; DWORD cUsageIdentifier; LPSTR *rgpszUsageIdentifier; } USAGE_MATCH_;
typedef struct { DWORD cbSize; USAGE_MATCH_ RequestedUsage; } CHAIN_PARA_;

#define MAX_DEPTH 10

/* One allocation: the context, its chain, the element pointers and the
 * elements */
typedef struct {
    CHAIN_CONTEXT_  ctx;
    SIMPLE_CHAIN_   simple;
    SIMPLE_CHAIN_  *simples[1];
    CHAIN_ELEMENT_ *ptrs[MAX_DEPTH];
    CHAIN_ELEMENT_  el[MAX_DEPTH];
    LONG            refs;
} Chain;

typedef struct { Cert *certs[MAX_DEPTH]; uint32_t flags[MAX_DEPTH]; int n; } Seen;

/* Mbed TLS calls this for each certificate of the chain it built, the
 * root (depth n) first */
static int seen_cb(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    Seen *s = p;
    if (depth >= 0 && depth < MAX_DEPTH) {
        if (depth + 1 > s->n) s->n = depth + 1;
        if (!s->certs[depth]) s->certs[depth] = cert_new(crt->raw.p, (DWORD)crt->raw.len);
        s->flags[depth] = *flags;
    }
    return 0;
}

static DWORD trust_bits(uint32_t f)
{
    DWORD e = 0;
    if (f & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) e |= TRUST_NOT_TIME_VALID;
    if (f & MBEDTLS_X509_BADCERT_REVOKED) e |= TRUST_IS_REVOKED;
    if (f & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK | MBEDTLS_X509_BADCERT_BAD_KEY))
        e |= TRUST_NOT_SIGNATURE_VALID;
    if (f & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE | MBEDTLS_X509_BADCERT_NS_CERT_TYPE))
        e |= TRUST_NOT_VALID_FOR_USAGE;
    if (f & MBEDTLS_X509_BADCERT_OTHER) e |= TRUST_INVALID_EXTENSION;
    return e;
}

/* Dotted OID text to its DER contents (no tag or length); length or 0 */
static size_t oid_der(const char *s, BYTE *out, size_t cap)
{
    unsigned long v[32];
    int n = 0;
    while (*s && n < 32) {
        unsigned long x = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') x = x * 10 + (unsigned long)(*s++ - '0');
        v[n++] = x;
        if (*s == '.') s++;
        else if (*s) return 0;
    }
    if (n < 2) return 0;
    size_t o = 0;
    for (int i = 1; i < n; i++) {
        unsigned long x = i == 1 ? v[0] * 40 + v[1] : v[i];
        BYTE tmp[8];
        int k = 0;
        do { tmp[k++] = (BYTE)(x & 0x7F); x >>= 7; } while (x && k < 8);
        if (o + (size_t)k > cap) return 0;
        while (k--) out[o++] = (BYTE)(tmp[k] | (k ? 0x80 : 0));
    }
    return o;
}

/* Whether the end certificate allows the requested usages (OR: any of
 * them; AND: all); no extended key usage extension allows all */
static BOOL usage_ok(const mbedtls_x509_crt *leaf, const CHAIN_PARA_ *para)
{
    if (!para || para->cbSize < sizeof(CHAIN_PARA_) || !para->RequestedUsage.cUsageIdentifier) return TRUE;
    BOOL any = FALSE, all = TRUE;
    for (DWORD i = 0; i < para->RequestedUsage.cUsageIdentifier; i++) {
        BYTE der[64];
        size_t n = para->RequestedUsage.rgpszUsageIdentifier[i] ?
                   oid_der(para->RequestedUsage.rgpszUsageIdentifier[i], der, sizeof(der)) : 0;
        BOOL ok = n && mbedtls_x509_crt_check_extended_key_usage(leaf, (const char *)der, n) == 0;
        any |= ok;
        all &= ok;
    }
    return para->RequestedUsage.dwType == 1 /* USAGE_MATCH_TYPE_OR */ ? any : all;
}

static BOOL same_name(const mbedtls_x509_buf *a, const mbedtls_x509_buf *b)
{
    return a->len == b->len && !memcmp(a->p, b->p, a->len);
}

CRYPT32API BOOL WINAPI CertGetCertificateChain(HANDLE engine, const void *cv, LPFILETIME t, HANDLE extra, const void *pv,
                                               DWORD flags, PVOID r, const void **out)
{
    (void)engine; (void)t; (void)r;
    const Cert *c = cv;
    const CHAIN_PARA_ *para = pv;
    if (out) *out = NULL;
    if (!c || !out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    /* The candidates: the certificate, then issuers from the extra store
     * and the certificate's own store, each after the one it signed */
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    if (mbedtls_x509_crt_parse_der(&chain, c->der, c->ctx.cbCertEncoded)) {
        mbedtls_x509_crt_free(&chain);
        SetLastError(CRYPT_E_ASN1_BADTAG_);
        return FALSE;
    }
    lock();
    mbedtls_x509_crt *last = &chain;
    for (int depth = 1; depth < MAX_DEPTH; depth++) {
        if (same_name(&last->issuer_raw, &last->subject_raw)) break;        /* self-issued: the top */
        mbedtls_x509_crt *found = NULL;
        Store *stores[2] = { store_of(extra), c->ctx.hCertStore ? store_of(c->ctx.hCertStore) : NULL };
        SetLastError(0);
        for (int k = 0; k < 2 && !found; k++)
            for (Cert *x = stores[k] ? next_in(stores[k], NULL) : NULL; x && !found; x = next_in(stores[k], x)) {
                if (x->info.Subject.cbData != last->issuer_raw.len ||
                    memcmp(x->info.Subject.pbData, last->issuer_raw.p, last->issuer_raw.len) ||
                    (x->ctx.cbCertEncoded == last->raw.len && !memcmp(x->der, last->raw.p, last->raw.len)))
                    continue;
                if (mbedtls_x509_crt_parse_der(&chain, x->der, x->ctx.cbCertEncoded) == 0) {
                    for (found = &chain; found->next; found = found->next) ;
                }
            }
        if (!found) break;
        last = found;
    }
    /* Checked one chain at a time: the roots load on first use, and two
     * threads' checks at once made signatures fail now and then (Steam
     * checks two connections' chains together) */
    Seen seen = { 0 };
    uint32_t vflags = 0;
    const mbedtls_x509_crt *trust = roots();
    mbedtls_x509_crt_verify(&chain, (mbedtls_x509_crt *)trust, NULL, NULL, &vflags, seen_cb, &seen);
    BOOL usage = usage_ok(&chain, para);
    unlock();
    if (!seen.n) {                                      /* (nothing verified: the certificate alone) */
        seen.certs[0] = cert_new(c->der, c->ctx.cbCertEncoded);
        seen.flags[0] = vflags;
        seen.n = 1;
    }
    mbedtls_x509_crt_free(&chain);

    Chain *ch = calloc(1, sizeof(*ch));
    if (!ch) {
        for (int i = 0; i < seen.n; i++) cert_unref(seen.certs[i]);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    ch->refs = 1;
    DWORD all = 0;
    BOOL revocation = (flags & 0xF0000000) != 0;        /* CERT_CHAIN_REVOCATION_CHECK_* */
    for (int i = 0; i < seen.n; i++) {
        CHAIN_ELEMENT_ *e = &ch->el[i];
        e->cbSize = sizeof(*e);
        e->pCertContext = seen.certs[i] ? &seen.certs[i]->ctx : NULL;
        e->TrustStatus.dwErrorStatus = trust_bits(seen.flags[i]);
        if (i == 0 && !usage) e->TrustStatus.dwErrorStatus |= TRUST_NOT_VALID_FOR_USAGE;
        if (revocation && (i + 1 < seen.n || !(flags & 0x40000000) /* ..._EXCLUDE_ROOT */))
            e->TrustStatus.dwErrorStatus |= TRUST_REVOCATION_UNKNOWN | TRUST_IS_OFFLINE_REVOCATION;
        if (i + 1 < seen.n) e->TrustStatus.dwInfoStatus = TRUST_HAS_NAME_MATCH_ISSUER;
        all |= e->TrustStatus.dwErrorStatus;
        ch->ptrs[i] = e;
    }
    /* The top: trusted, an untrusted self-signed root, or short of one */
    CHAIN_ELEMENT_ *top = &ch->el[seen.n - 1];
    const Cert *tc = seen.certs[seen.n - 1];
    BOOL self = tc && tc->info.Subject.cbData == tc->info.Issuer.cbData &&
                !memcmp(tc->info.Subject.pbData, tc->info.Issuer.pbData, tc->info.Subject.cbData);
    if (self) top->TrustStatus.dwInfoStatus |= TRUST_IS_SELF_SIGNED;
    if (vflags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        DWORD e = self ? TRUST_IS_UNTRUSTED_ROOT : TRUST_IS_PARTIAL_CHAIN;
        top->TrustStatus.dwErrorStatus |= e;
        all |= e;
    }
    if (!(vflags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)) ch->ctx.TrustStatus.dwInfoStatus |= TRUST_HAS_PREFERRED_ISSUER;
    ch->simple.cbSize = sizeof(ch->simple);
    ch->simple.TrustStatus.dwErrorStatus = all;
    ch->simple.TrustStatus.dwInfoStatus = ch->ctx.TrustStatus.dwInfoStatus;
    ch->simple.cElement = (DWORD)seen.n;
    ch->simple.rgpElement = ch->ptrs;
    ch->simples[0] = &ch->simple;
    ch->ctx.cbSize = sizeof(ch->ctx);
    ch->ctx.TrustStatus.dwErrorStatus = all;
    ch->ctx.cChain = 1;
    ch->ctx.rgpChain = ch->simples;
    ch->ctx.dwCreateFlags = flags;
    *out = ch;
    return TRUE;
}

CRYPT32API const void *WINAPI CertDuplicateCertificateChain(const void *cv)
{
    Chain *ch = (Chain *)cv;
    if (ch) InterlockedIncrement(&ch->refs);
    return ch;
}

CRYPT32API VOID WINAPI CertFreeCertificateChain(const void *cv)
{
    Chain *ch = (Chain *)cv;
    if (!ch || InterlockedDecrement(&ch->refs)) return;
    for (DWORD i = 0; i < ch->simple.cElement; i++)
        if (ch->el[i].pCertContext) cert_unref((Cert *)ch->el[i].pCertContext);
    free(ch);
}

/* CertVerifyCertificateChainPolicy for the base and SSL policies: the
 * chain's first error as the HRESULT Windows gives it, and for SSL the
 * server name against the end certificate */
typedef struct { DWORD cbSize, dwFlags; void *pvExtraPolicyPara; } POLICY_PARA_;
typedef struct { DWORD cbSize, dwError; LONG lChainIndex, lElementIndex; void *pvExtraPolicyStatus; } POLICY_STATUS_;
typedef struct { DWORD cbSize, dwAuthType, fdwChecks; WCHAR *pwszServerName; } SSL_POLICY_PARA_;

CRYPT32API BOOL WINAPI CertVerifyCertificateChainPolicy(LPCSTR policy, const void *cv, POLICY_PARA_ *pp, POLICY_STATUS_ *ps)
{
    const CHAIN_CONTEXT_ *ch = cv;
    if (!ch || !ps) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG_PTR kind = (ULONG_PTR)policy;
    if (kind != 1 /* BASE */ && kind != 4 /* SSL */) { SetLastError(CRYPT_E_NOT_FOUND_); return FALSE; }
    DWORD e = ch->TrustStatus.dwErrorStatus & ~(TRUST_REVOCATION_UNKNOWN | TRUST_IS_OFFLINE_REVOCATION);
    ps->lChainIndex = ps->lElementIndex = -1;
    ps->dwError = e & TRUST_IS_UNTRUSTED_ROOT ? 0x800B0109 :        /* CERT_E_UNTRUSTEDROOT */
                  e & TRUST_IS_PARTIAL_CHAIN ? 0x800B010A :         /* CERT_E_CHAINING */
                  e & TRUST_NOT_TIME_VALID ? 0x800B0101 :           /* CERT_E_EXPIRED */
                  e & TRUST_IS_REVOKED ? 0x80092010 :               /* CRYPT_E_REVOKED */
                  e & TRUST_NOT_VALID_FOR_USAGE ? 0x800B0110 :      /* CERT_E_WRONG_USAGE */
                  e & TRUST_NOT_SIGNATURE_VALID ? 0x80096004 :      /* TRUST_E_CERT_SIGNATURE */
                  e ? 0x800B010B : 0;                               /* CERT_E_CRITICAL... */
    if (ps->dwError) ps->lChainIndex = ps->lElementIndex = 0;
    const SSL_POLICY_PARA_ *ssl = kind == 4 && pp && pp->cbSize >= sizeof(*pp) ? pp->pvExtraPolicyPara : NULL;
    if (!ps->dwError && ssl && ssl->pwszServerName && ssl->pwszServerName[0] && ch->cChain && ch->rgpChain[0]->cElement) {
        const Cert *leaf = (const Cert *)ch->rgpChain[0]->rgpElement[0]->pCertContext;
        char host[256];
        int i = 0;
        for (; ssl->pwszServerName[i] && i < 255; i++) host[i] = (char)ssl->pwszServerName[i];
        host[i] = 0;
        mbedtls_x509_crt x;
        mbedtls_x509_crt_init(&x);
        uint32_t f = 0;
        lock();
        if (leaf && !mbedtls_x509_crt_parse_der(&x, leaf->der, leaf->ctx.cbCertEncoded))
            mbedtls_x509_crt_verify(&x, &x, NULL, host, &f, NULL, NULL);
        unlock();
        if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) { ps->dwError = 0x800B010F; ps->lChainIndex = ps->lElementIndex = 0; }  /* CERT_E_CN_NO_MATCH */
        mbedtls_x509_crt_free(&x);
    }
    return TRUE;
}
