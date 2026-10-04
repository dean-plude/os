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
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crl.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "tls_glue.h"
#include "crypt32_int.h"
#include "x509_internal.h"
#include <stdio.h>
#include <wchar.h>

#define CRYPT32API __declspec(dllexport)
#define CRYPT_E_NOT_FOUND_    0x80092004L
#define CRYPT_E_EXISTS_       0x80092005L
#define CRYPT_E_ASN1_BADTAG_  0x8009310BL

#define X509_ASN_ENCODING_    0x1
#define CERT_STORE_PROV_MSG_         1
#define CERT_STORE_PROV_MEMORY_      2
#define CERT_STORE_PROV_PKCS7_       5
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
    BOOL    sysroot;                    /* the system ROOT store: changes persist */
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

void crypt32_init(void) { roots(); }

/* Roots added to or deleted from the system ROOT store while running: the
 * trusted list is then rebuilt from the roots loaded at start, less the
 * deleted ones, plus the added ones.  An added root is kept in
 * C:\Windows\System32\CertStore (where the roots trusted with certutil
 * are), so later programs load it at start; deleting it removes the file.
 * (Windows keeps them in the registry and asks the user first.) */
#define CERTSTORE "C:\\Windows\\System32\\CertStore"
typedef struct { BYTE *der; DWORD n; } Der;
static Der   *g_added;
static DWORD  g_nadded;
static BYTE (*g_removed)[20];
static DWORD  g_nremoved;
static LONG   g_trust_gen, g_built_gen;
static mbedtls_x509_crt g_trust;
static BOOL   g_trust_built;

static BOOL is_removed(const BYTE *der, size_t n)
{
    BYTE h[20];
    mbedtls_sha1(der, n, h);
    for (DWORD i = 0; i < g_nremoved; i++) if (!memcmp(g_removed[i], h, 20)) return TRUE;
    return FALSE;
}

/* The trusted roots (under the lock) */
static const mbedtls_x509_crt *trusted(void)
{
    const mbedtls_x509_crt *r = roots();
    if (!g_trust_gen || !r) return r;
    if (g_built_gen != g_trust_gen) {
        if (g_trust_built) mbedtls_x509_crt_free(&g_trust);
        mbedtls_x509_crt_init(&g_trust);
        g_trust_built = TRUE;
        for (const mbedtls_x509_crt *x = r; x && x->raw.p; x = x->next)
            if (!is_removed(x->raw.p, x->raw.len)) mbedtls_x509_crt_parse_der(&g_trust, x->raw.p, x->raw.len);
        for (DWORD i = 0; i < g_nadded; i++) mbedtls_x509_crt_parse_der(&g_trust, g_added[i].der, g_added[i].n);
        g_built_gen = g_trust_gen;
    }
    return &g_trust;
}

static void store_path(const BYTE *der, DWORD n, char *path)
{
    BYTE h[20];
    mbedtls_sha1(der, n, h);
    int o = sprintf(path, CERTSTORE "\\");
    for (int i = 0; i < 20; i++) o += sprintf(path + o, "%02X", h[i]);
    strcpy(path + o, ".cer");
}

static void trust_root(const BYTE *der, DWORD n)
{
    lock();
    BYTE h[20];
    mbedtls_sha1(der, n, h);
    for (DWORD i = 0; i < g_nremoved; i++)
        if (!memcmp(g_removed[i], h, 20)) { memmove(g_removed[i], g_removed[i + 1], (g_nremoved - i - 1) * 20); g_nremoved--; break; }
    BOOL have = FALSE;
    for (const mbedtls_x509_crt *x = trusted(); x && x->raw.p && !have; x = x->next)
        have = x->raw.len == n && !memcmp(x->raw.p, der, n);
    if (!have) {
        Der *a = realloc(g_added, (g_nadded + 1) * sizeof(Der));
        BYTE *copy = malloc(n);
        if (a) g_added = a;
        if (a && copy) { memcpy(copy, der, n); g_added[g_nadded].der = copy; g_added[g_nadded++].n = n; }
        else free(copy);
        char path[MAX_PATH];
        CreateDirectoryA(CERTSTORE, NULL);
        store_path(der, n, path);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, der, n, &w, NULL); CloseHandle(f); }
    }
    g_trust_gen++;
    unlock();
}

static void distrust_root(const BYTE *der, DWORD n)
{
    lock();
    for (DWORD i = 0; i < g_nadded; i++)
        if (g_added[i].n == n && !memcmp(g_added[i].der, der, n)) {
            free(g_added[i].der);
            g_added[i] = g_added[--g_nadded];
            break;
        }
    void *r = realloc(g_removed, (g_nremoved + 1) * 20);
    if (r) { g_removed = r; mbedtls_sha1(der, n, g_removed[g_nremoved++]); }
    /* the file it was kept in, whatever its name (certutil names them) */
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(CERTSTORE "\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || fd.nFileSizeLow != n) continue;
            char path[MAX_PATH];
            snprintf(path, sizeof(path), CERTSTORE "\\%s", fd.cFileName);
            HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            BYTE *buf = malloc(n);
            DWORD got = 0;
            BOOL same = f != INVALID_HANDLE_VALUE && buf && ReadFile(f, buf, n, &got, NULL) && got == n && !memcmp(buf, der, n);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            free(buf);
            if (same) DeleteFileA(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    g_trust_gen++;
    unlock();
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
    s->sysroot = TRUE;
    for (const mbedtls_x509_crt *r = trusted(); r && r->raw.p; r = r->next) {
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
    if (kind == CERT_STORE_PROV_MSG_ || kind == CERT_STORE_PROV_PKCS7_) {
        /* a signed message's certificates */
        HANDLE msg = kind == CERT_STORE_PROV_MSG_ ? (HANDLE)para :
                     para ? crypt32_msg_from_blob(((const BLOB_ *)para)->pbData, ((const BLOB_ *)para)->cbData) : 0;
        if (!msg) { if (!para) SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        Store *s = open_store(FALSE);
        DWORD n, count = crypt32_msg_cert_count(msg);
        for (DWORD i = 0; s && i < count; i++) {
            const BYTE *der = crypt32_msg_cert(msg, i, &n);
            if (der) crypt32_store_add_der(s, der, n, NULL);
        }
        if (kind == CERT_STORE_PROV_PKCS7_) CryptMsgClose(msg);
        return s;
    }
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
        if (s->certs[i]->ctx.cbCertEncoded == c->ctx.cbCertEncoded && !memcmp(s->certs[i]->der, c->ctx.pbCertEncoded, c->ctx.cbCertEncoded))
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
        if (ok && s->sysroot) trust_root(c->der, c->ctx.cbCertEncoded);
    }
    unlock();
    return ok;
}

HANDLE crypt32_memory_store(void) { return open_store(FALSE); }

BOOL crypt32_store_add_der(HANDLE store, const BYTE *der, DWORD n, const void **out)
{
    return CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING_, der, n, CERT_STORE_ADD_USE_EXISTING_, out);
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
                if (s->sysroot) distrust_root(c->der, c->ctx.cbCertEncoded);
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

/* Two little-endian integers, equal whatever their sign-extension bytes */
CRYPT32API BOOL WINAPI CertCompareIntegerBlob(const BLOB_ *a, const BLOB_ *b)
{
    if (!a || !b) return FALSE;
    DWORD na = a->cbData, nb = b->cbData;
    while (na > 1 && a->pbData[na - 1] == 0 && !(a->pbData[na - 2] & 0x80)) na--;
    while (nb > 1 && b->pbData[nb - 1] == 0 && !(b->pbData[nb - 2] & 0x80)) nb--;
    return na == nb && !memcmp(a->pbData, b->pbData, na);
}

/* CERT_FIND_ANY, CERT_FIND_EXISTING, CERT_FIND_SUBJECT_NAME and
 * CERT_FIND_ISSUER_NAME (encoded names), CERT_FIND_SHA1_HASH,
 * CERT_FIND_SUBJECT_CERT (issuer and serial number) */
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
        } else if (cmp == 11) {                                                 /* SUBJECT_CERT: issuer and serial */
            const CERT_INFO_ *ci = para;
            hit = ci && ci->Issuer.cbData == c->info.Issuer.cbData &&
                  !memcmp(ci->Issuer.pbData, c->info.Issuer.pbData, ci->Issuer.cbData) &&
                  CertCompareIntegerBlob(&ci->SerialNumber, &c->info.SerialNumber);
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

CRYPT32API const void *WINAPI CertGetSubjectCertificateFromStore(HANDLE h, DWORD enc, CERT_INFO_ *id)
{
    if (!id) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return CertFindCertificateInStore(h, enc, 0, 11 << 16 /* CERT_FIND_SUBJECT_CERT */, id, NULL);
}

/* CertControlStore: a store here always shows what is in it now (changes
 * to the system ROOT store are written as they are made), so resyncing and
 * committing have nothing to do; a change event is accepted, and since no
 * other program's changes reach a store while it is open, it stays unset */
CRYPT32API BOOL WINAPI CertControlStore(HANDLE h, DWORD flags, DWORD ctrl, const void *para)
{
    (void)flags; (void)para;
    if (!store_of(h)) return FALSE;
    if (ctrl >= 1 && ctrl <= 5) return TRUE;  /* RESYNC, NOTIFY_CHANGE, COMMIT, AUTO_RESYNC, CANCEL_NOTIFY */
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* -1 before the certificate's validity, 1 after it, 0 within */
CRYPT32API LONG WINAPI CertVerifyTimeValidity(const FILETIME *t, const CERT_INFO_ *ci)
{
    FILETIME now;
    if (!t) { GetSystemTimeAsFileTime(&now); t = &now; }
    if (CompareFileTime(t, &ci->NotBefore) < 0) return -1;
    if (CompareFileTime(t, &ci->NotAfter) > 0) return 1;
    return 0;
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

/* Names compare as their encodings, as Windows compares them */
CRYPT32API BOOL WINAPI CertCompareCertificateName(DWORD enc, const BLOB_ *a, const BLOB_ *b)
{
    (void)enc;
    return a && b && a->cbData == b->cbData && !memcmp(a->pbData, b->pbData, a->cbData);
}

/* CryptVerifyCertificateSignatureEx: whether @subject (a certificate, or
 * the encoding of a certificate or CRL) was signed with the key of
 * @issuer (a certificate, the first certificate of a chain, or a
 * CERT_PUBLIC_KEY_INFO); NTE_BAD_SIGNATURE if not */
#define NTE_BAD_SIGNATURE_    0x80090006L
#define NTE_BAD_ALGID_C       0x80090008L
#define SIGN_SUBJECT_BLOB_    1
#define SIGN_SUBJECT_CERT_    2
#define SIGN_ISSUER_PUBKEY_   1
#define SIGN_ISSUER_CERT_     2
#define SIGN_ISSUER_CHAIN_    3

/* SubjectPublicKeyInfo's DER from CERT_PUBLIC_KEY_INFO, for Mbed TLS */
static int spki_key(const PUBKEY_INFO_ *k, mbedtls_pk_context *pk)
{
    BYTE oid[64], buf[4096 + 128];
    mbedtls_asn1_buf o = { 0 };
    if (!k || !k->Algorithm.pszObjId || mbedtls_oid_from_numeric_string(&o, k->Algorithm.pszObjId, strlen(k->Algorithm.pszObjId)))
        return -1;
    size_t on = o.len;
    if (on > sizeof(oid)) { free(o.p); return -1; }
    memcpy(oid, o.p, on);
    free(o.p);
    size_t pn = k->Algorithm.Parameters.cbData, kn = k->PublicKey.cbData;
    if (pn + kn + 64 > sizeof(buf) - 16) return -1;
    /* SEQUENCE { SEQUENCE { OID, params }, BIT STRING } with long-form lengths */
    BYTE *p = buf;
    size_t alg = 2 + on + pn, bits = 1 + kn, alg_hdr = 4, bits_hdr = 4, body = alg_hdr + alg + bits_hdr + bits;
    *p++ = 0x30; *p++ = 0x82; *p++ = (BYTE)(body >> 8); *p++ = (BYTE)body;
    *p++ = 0x30; *p++ = 0x82; *p++ = (BYTE)(alg >> 8); *p++ = (BYTE)alg;
    *p++ = 0x06; *p++ = (BYTE)on; memcpy(p, oid, on); p += on;
    if (pn) { memcpy(p, k->Algorithm.Parameters.pbData, pn); p += pn; }
    *p++ = 0x03; *p++ = 0x82; *p++ = (BYTE)(bits >> 8); *p++ = (BYTE)bits;
    *p++ = (BYTE)k->PublicKey.cUnusedBits; memcpy(p, k->PublicKey.pbData, kn); p += kn;
    return mbedtls_pk_parse_public_key(pk, buf, (size_t)(p - buf));
}

CRYPT32API BOOL WINAPI CryptVerifyCertificateSignatureEx(ULONG_PTR prov, DWORD enc, DWORD stype, void *subject, DWORD itype,
                                                         void *issuer, DWORD flags, void *reserved)
{
    (void)prov; (void)enc; (void)flags; (void)reserved;
    const BYTE *der;
    size_t n;
    if (!subject) { SetLastError(E_INVALIDARG); return FALSE; }
    if (stype == SIGN_SUBJECT_CERT_) { const Cert *c = subject; der = c->der; n = c->ctx.cbCertEncoded; }
    else if (stype == SIGN_SUBJECT_BLOB_) { const BLOB_ *b = subject; der = b->pbData; n = b->cbData; }
    else { SetLastError(E_INVALIDARG); return FALSE; }
    crypt32_init();
    /* what was signed, how, and the signature */
    mbedtls_x509_crt crt;
    mbedtls_x509_crl crl;
    mbedtls_x509_crt_init(&crt);
    mbedtls_x509_crl_init(&crl);
    const mbedtls_x509_buf *tbs, *sig;
    mbedtls_md_type_t md;
    mbedtls_pk_type_t pkt;
    const void *opts;
    if (!mbedtls_x509_crt_parse_der(&crt, der, n)) {
        tbs = &crt.tbs; sig = &crt.sig; md = crt.MBEDTLS_PRIVATE(sig_md); pkt = crt.MBEDTLS_PRIVATE(sig_pk); opts = crt.MBEDTLS_PRIVATE(sig_opts);
    } else if (stype == SIGN_SUBJECT_BLOB_ && !mbedtls_x509_crl_parse_der(&crl, der, n)) {
        tbs = &crl.tbs; sig = &crl.sig; md = crl.MBEDTLS_PRIVATE(sig_md); pkt = crl.MBEDTLS_PRIVATE(sig_pk); opts = crl.MBEDTLS_PRIVATE(sig_opts);
    } else {
        mbedtls_x509_crt_free(&crt);
        SetLastError(CRYPT_E_ASN1_BADTAG_);
        return FALSE;
    }
    /* the issuer's key */
    mbedtls_pk_context key;
    mbedtls_x509_crt icrt;
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&icrt);
    mbedtls_pk_context *pk = NULL;
    const Cert *ic = NULL;
    if (itype == SIGN_ISSUER_CERT_) ic = issuer;
    else if (itype == SIGN_ISSUER_CHAIN_ && issuer) {     /* the chain's own certificate */
        const CHAIN_CONTEXT_ *ch = issuer;
        if (ch->cChain && ch->rgpChain[0]->cElement) ic = (const Cert *)ch->rgpChain[0]->rgpElement[0]->pCertContext;
    }
    if (ic && !mbedtls_x509_crt_parse_der(&icrt, ic->der, ic->ctx.cbCertEncoded)) pk = &icrt.pk;
    else if (itype == SIGN_ISSUER_PUBKEY_ && !spki_key(issuer, &key)) pk = &key;
    DWORD err = 0;
    if (!pk) err = itype == SIGN_ISSUER_CERT_ || itype == SIGN_ISSUER_PUBKEY_ ? (DWORD)CRYPT_E_ASN1_BADTAG_ : (DWORD)E_INVALIDARG;
    else {
        const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(md);
        BYTE hash[MBEDTLS_MD_MAX_SIZE];
        if (!mi || mbedtls_md(mi, tbs->p, tbs->len, hash)) err = (DWORD)NTE_BAD_ALGID_C;
        else if (mbedtls_pk_verify_ext(pkt, opts, pk, md, hash, mbedtls_md_get_size(mi), sig->p, sig->len))
            err = (DWORD)NTE_BAD_SIGNATURE_;
    }
    mbedtls_x509_crt_free(&icrt);
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    mbedtls_x509_crl_free(&crl);
    if (err) { SetLastError(err); return FALSE; }
    return TRUE;
}

typedef struct { DWORD cUsageIdentifier; LPSTR *rgpszUsageIdentifier; } ENHKEY_USAGE_;
typedef struct { DWORD dwType; ENHKEY_USAGE_ Usage; } USAGE_MATCH_;
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
    if (!para || para->cbSize < sizeof(CHAIN_PARA_) || !para->RequestedUsage.Usage.cUsageIdentifier) return TRUE;
    BOOL any = FALSE, all = TRUE;
    for (DWORD i = 0; i < para->RequestedUsage.Usage.cUsageIdentifier; i++) {
        BYTE der[64];
        size_t n = para->RequestedUsage.Usage.rgpszUsageIdentifier[i] ?
                   oid_der(para->RequestedUsage.Usage.rgpszUsageIdentifier[i], der, sizeof(der)) : 0;
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

/* Signatures Windows' chain engine accepts: SHA-1 and the SHA-2 family,
 * RSA keys of 1024 bits and up, any curve (Mbed TLS's default profile
 * refuses SHA-1, which older Authenticode chains are signed with) */
static const mbedtls_x509_crt_profile g_profile = {
    MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA1) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA224) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA256) |
    MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA384) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA512),
    0xFFFFFFF, 0xFFFFFFF, 1024
};

CRYPT32API BOOL WINAPI CertGetCertificateChain(HANDLE engine, const void *cv, LPFILETIME t, HANDLE extra, const void *pv,
                                               DWORD flags, PVOID r, const void **out)
{
    (void)engine; (void)r;
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
    const mbedtls_x509_crt *trust = trusted();
    mbedtls_x509_crt_verify_with_profile(&chain, (mbedtls_x509_crt *)trust, NULL, &g_profile, NULL, &vflags, seen_cb, &seen);
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
    for (int i = 0; t && i < seen.n; i++) {
        /* valid at the time asked about (a signature's timestamp), not now */
        seen.flags[i] &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        LONG v = seen.certs[i] ? CertVerifyTimeValidity(t, &seen.certs[i]->info) : 0;
        if (v) seen.flags[i] |= v > 0 ? MBEDTLS_X509_BADCERT_EXPIRED : MBEDTLS_X509_BADCERT_FUTURE;
    }
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

/* The Microsoft root policy's keys: SHA-256 of each root's public key (the
 * RSAPublicKey in its SubjectPublicKeyInfo), as Windows lists them.  The
 * product roots always count; the application root ("Microsoft Root
 * Certificate Authority 2011", which signs Microsoft's code signing PCAs
 * 2011 and 2024) only with MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG.
 * Microsoft's test and flight roots are not in NovaOS's ROOT store, so
 * their flags change nothing. */
static const BYTE g_ms_roots[][32] = {
    /* Microsoft Root Authority (1997) */
    { 0xee,0x09,0xb0,0x7a,0x85,0xe8,0xf2,0x4a,0x01,0xef,0x63,0x1a,0xe6,0x71,0xfe,0xf8,
      0xde,0xa8,0x01,0x5a,0x09,0xa7,0x15,0xe6,0xa6,0x73,0x90,0x11,0x90,0x92,0xb8,0x16 },
    /* Microsoft Root Certificate Authority (2001) */
    { 0x60,0xbd,0xed,0x75,0xc5,0xfd,0x11,0x90,0x10,0xd6,0x83,0x2f,0x76,0xde,0xfc,0x39,
      0x34,0x73,0xd7,0xa0,0xce,0x64,0xfb,0xd6,0x8d,0xab,0xa2,0x9b,0xfd,0x0b,0x2f,0x7c },
    /* Microsoft Root Certificate Authority 2010 */
    { 0x12,0xeb,0x31,0xfd,0xc8,0x92,0x49,0xa0,0xeb,0x67,0xeb,0x65,0xc2,0x97,0x7d,0xbe,
      0x2a,0xd9,0x6a,0x90,0x9c,0xcb,0xd1,0x80,0xf7,0xe2,0xe1,0x6b,0x27,0x82,0xca,0xee },
};
static const BYTE g_ms_app_root[32] = {
    /* Microsoft Root Certificate Authority 2011 */
    0x4a,0xbb,0x05,0x94,0xd3,0x03,0xef,0x70,0x77,0x13,0x88,0x34,0xab,0x31,0x5e,0x94,
    0x1e,0x96,0x30,0x93,0xe0,0x5b,0x4b,0x14,0xaf,0x5d,0xcb,0x52,0x77,0x12,0xc0,0x0a,
};
#define MS_ROOT_CHECK_APPLICATION_ROOT_ 0x00020000

/* CERT_CHAIN_POLICY_MICROSOFT_ROOT: whether the last element of the first
 * simple chain carries one of Microsoft's root keys; CERT_E_UNTRUSTEDROOT
 * at that element if not.  As on Windows it judges the root's key only:
 * the chain's own errors are the base and Authenticode policies' business. */
static void ms_root_policy(const CHAIN_CONTEXT_ *ch, DWORD flags, POLICY_STATUS_ *ps)
{
    const SIMPLE_CHAIN_ *sc = ch->cChain ? ch->rgpChain[0] : NULL;
    const CERT_CONTEXT_ *root = sc && sc->cElement ? sc->rgpElement[sc->cElement - 1]->pCertContext : NULL;
    BOOL ok = FALSE;
    if (root && root->pCertInfo) {
        const BIT_BLOB_ *key = &root->pCertInfo->SubjectPublicKeyInfo.PublicKey;
        BYTE h[32];
        mbedtls_sha256(key->pbData, key->cbData, h, 0);
        for (size_t i = 0; i < sizeof(g_ms_roots) / sizeof(g_ms_roots[0]) && !ok; i++)
            ok = !memcmp(h, g_ms_roots[i], 32);
        if (!ok && (flags & MS_ROOT_CHECK_APPLICATION_ROOT_)) ok = !memcmp(h, g_ms_app_root, 32);
    }
    ps->dwError = ok ? 0 : 0x800B0109;                              /* CERT_E_UNTRUSTEDROOT */
    ps->lChainIndex = ok ? -1 : 0;
    ps->lElementIndex = ok ? -1 : sc && sc->cElement ? (LONG)sc->cElement - 1 : 0;
}

CRYPT32API BOOL WINAPI CertVerifyCertificateChainPolicy(LPCSTR policy, const void *cv, POLICY_PARA_ *pp, POLICY_STATUS_ *ps)
{
    const CHAIN_CONTEXT_ *ch = cv;
    if (!ch || !ps) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG_PTR kind = (ULONG_PTR)policy;
    if (kind == 7 /* MICROSOFT_ROOT */) {
        ms_root_policy(ch, pp && pp->cbSize >= 8 ? pp->dwFlags : 0, ps);
        return TRUE;
    }
    if (kind != 1 /* BASE */ && kind != 2 /* AUTHENTICODE */ && kind != 3 /* AUTHENTICODE_TS */ && kind != 4 /* SSL */) {
        SetLastError(CRYPT_E_NOT_FOUND_);
        return FALSE;
    }
    DWORD e = ch->TrustStatus.dwErrorStatus, pf = pp && pp->cbSize >= 8 ? pp->dwFlags : 0;
    /* Authenticode: a revocation check that was asked for and could not be
     * made (offline) fails, unless the caller ignores unknown revocation */
    BOOL rev_failed = (kind == 2 || kind == 3) && (e & TRUST_REVOCATION_UNKNOWN) && !(pf & 0x00000F00);
    e &= ~(TRUST_REVOCATION_UNKNOWN | TRUST_IS_OFFLINE_REVOCATION);
    if (pf & 0x00000007) e &= ~TRUST_NOT_TIME_VALID;               /* CERT_CHAIN_POLICY_IGNORE_*_NOT_TIME_VALID_FLAG */
    if (pf & 0x00000010) e &= ~TRUST_IS_UNTRUSTED_ROOT;            /* CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG */
    if (pf & 0x00000020) e &= ~TRUST_NOT_VALID_FOR_USAGE;          /* CERT_CHAIN_POLICY_IGNORE_WRONG_USAGE_FLAG */
    ps->lChainIndex = ps->lElementIndex = -1;
    ps->dwError = e & TRUST_NOT_SIGNATURE_VALID ? 0x80096004 :     /* TRUST_E_CERT_SIGNATURE */
                  e & TRUST_IS_UNTRUSTED_ROOT ? 0x800B0109 :        /* CERT_E_UNTRUSTEDROOT */
                  e & TRUST_IS_PARTIAL_CHAIN ? 0x800B010A :         /* CERT_E_CHAINING */
                  e & TRUST_NOT_TIME_VALID ? 0x800B0101 :           /* CERT_E_EXPIRED */
                  e & TRUST_IS_REVOKED ? 0x80092010 :               /* CRYPT_E_REVOKED */
                  e & TRUST_NOT_VALID_FOR_USAGE ? 0x800B0110 :      /* CERT_E_WRONG_USAGE */
                  e ? 0x800B010B :                                  /* CERT_E_CRITICAL... */
                  rev_failed ? 0x800B010E : 0;                      /* CERT_E_REVOCATION_FAILURE */
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

/* -----------------------------------------------------------------------
 * Names as text
 * ----------------------------------------------------------------------- */
static const struct { const char *oid, *key; } g_keys[] = {
    { "2.5.4.3", "CN" }, { "2.5.4.11", "OU" }, { "2.5.4.10", "O" }, { "2.5.4.7", "L" }, { "2.5.4.8", "S" },
    { "2.5.4.6", "C" }, { "2.5.4.9", "STREET" }, { "2.5.4.5", "SERIALNUMBER" }, { "2.5.4.4", "SN" },
    { "2.5.4.42", "G" }, { "2.5.4.12", "T" }, { "2.5.4.43", "I" }, { "1.2.840.113549.1.9.1", "E" },
    { "0.9.2342.19200300.100.1.25", "DC" }, { "0.9.2342.19200300.100.1.1", "UID" },
};

/* An attribute's value as UTF-16 into @out (@cap characters); its length */
static size_t value_text(const mbedtls_x509_buf *v, WCHAR *out, size_t cap)
{
    size_t o = 0;
    if (v->tag == 0x1E) {                                       /* BMPString: UCS-2, big-endian */
        for (size_t i = 0; i + 1 < v->len; i += 2) { if (o < cap) out[o] = (WCHAR)(v->p[i] << 8 | v->p[i + 1]); o++; }
    } else if (v->tag == 0x1C) {                                /* UniversalString: UCS-4 */
        for (size_t i = 0; i + 3 < v->len; i += 4) { if (o < cap) out[o] = (WCHAR)(v->p[i + 2] << 8 | v->p[i + 3]); o++; }
    } else if (v->tag == 0x0C) {                                /* UTF8String */
        int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)v->p, (int)v->len, NULL, 0);
        if (n > 0 && (size_t)n <= cap) MultiByteToWideChar(CP_UTF8, 0, (const char *)v->p, (int)v->len, out, n);
        o = n > 0 ? (size_t)n : 0;
    } else {                                                    /* Printable, IA5, Teletex: bytes */
        for (size_t i = 0; i < v->len; i++) { if (o < cap) out[o] = v->p[i]; o++; }
    }
    return o;
}

/* An encoded Name parsed into Mbed TLS's list (free with name_free) */
static BOOL name_parse(const BYTE *der, DWORD n, mbedtls_x509_name *out)
{
    unsigned char *p = (unsigned char *)der;
    size_t len;
    memset(out, 0, sizeof(*out));
    if (!der || mbedtls_asn1_get_tag(&p, der + n, &len, 0x30)) return FALSE;
    if (!len) return TRUE;
    return mbedtls_x509_get_name(&p, p + len, out) == 0;
}
static void name_free(mbedtls_x509_name *n) { mbedtls_asn1_free_named_data_list_shallow(n->next); }

static BOOL oid_eq(const mbedtls_x509_buf *oid, const char *text)
{
    char s[64];
    oid_text(oid, s, sizeof(s));
    return !strcmp(s, text);
}

/* Text written into a caller's buffer of @cap characters, as the Cert*Str
 * functions return it: the count with the terminator, truncated to fit */
typedef struct { WCHAR *buf; size_t cap, n; } Text;
static void text_w(Text *t, const WCHAR *s, size_t n) { for (size_t i = 0; i < n; i++, t->n++) if (t->buf && t->n + 1 < t->cap) t->buf[t->n] = s[i]; }
static void text_a(Text *t, const char *s) { for (; *s; s++, t->n++) if (t->buf && t->n + 1 < t->cap) t->buf[t->n] = (BYTE)*s; }
static DWORD text_end(Text *t)
{
    if (t->buf && t->cap) t->buf[t->n + 1 < t->cap ? t->n : t->cap - 1] = 0;
    return (DWORD)(t->buf && t->n + 1 > t->cap ? t->cap : t->n + 1);
}

static void put_value(Text *t, const mbedtls_x509_buf *v, BOOL quote)
{
    WCHAR tmp[512];
    size_t n = value_text(v, tmp, 512);
    if (n > 512) n = 512;
    BOOL q = FALSE;
    for (size_t i = 0; quote && i < n; i++)
        if (wcschr(L",+=\"\n<>#;", tmp[i]) || (i == 0 || i == n - 1) && tmp[i] == ' ') q = TRUE;
    if (q) text_a(t, "\"");
    for (size_t i = 0; i < n; i++) { if (q && tmp[i] == '"') text_a(t, "\""); text_w(t, tmp + i, 1); }
    if (q) text_a(t, "\"");
}

/* CertNameToStr: CERT_SIMPLE_NAME_STR (values), CERT_OID_NAME_STR
 * (OID=value), CERT_X500_NAME_STR (CN=value), in encoded order unless
 * CERT_NAME_STR_REVERSE_FLAG */
static DWORD name_to_str(const BLOB_ *name, DWORD type, Text *t)
{
    mbedtls_x509_name nm;
    if (name && name_parse(name->pbData, name->cbData, &nm) && nm.oid.p) {
        const mbedtls_x509_name *list[64];
        int n = 0;
        for (const mbedtls_x509_name *x = &nm; x && n < 64; x = x->next) list[n++] = x;
        BOOL rev = (type & 0x02000000) != 0;
        const char *sep = type & 0x40000000 ? "; " : type & 0x08000000 ? "\r\n" : ", ";
        for (int k = 0; k < n; k++) {
            const mbedtls_x509_name *x = list[rev ? n - 1 - k : k];
            if (k) {
                const mbedtls_x509_name *prev = list[rev ? n - k : k - 1];
                text_a(t, (rev ? x : prev)->next_merged && !(type & 0x20000000) ? " + " : sep);
            }
            DWORD kind = type & 0xFF;
            if (kind != 1) {
                char oid[64];
                oid_text(&x->oid, oid, sizeof(oid));
                const char *key = oid;
                if (kind == 3) for (size_t i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) if (!strcmp(oid, g_keys[i].oid)) key = g_keys[i].key;
                text_a(t, key);
                text_a(t, "=");
            }
            put_value(t, &x->val, !(type & 0x10000000));
        }
        name_free(&nm);
    }
    return text_end(t);
}

CRYPT32API DWORD WINAPI CertNameToStrW(DWORD enc, const BLOB_ *name, DWORD type, LPWSTR s, DWORD n)
{
    (void)enc;
    Text t = { s, n, 0 };
    return name_to_str(name, type, &t);
}

/* The A forms: the W text in the ANSI code page */
static DWORD narrow(DWORD (*w)(void *, WCHAR *, DWORD), void *ctx, LPSTR s, DWORD n)
{
    DWORD need = w(ctx, NULL, 0);
    WCHAR *tmp = malloc(need * sizeof(WCHAR));
    if (!tmp) { if (s && n) s[0] = 0; return 1; }
    w(ctx, tmp, need);
    int k = WideCharToMultiByte(CP_ACP, 0, tmp, -1, NULL, 0, NULL, NULL);
    if (s && n) {
        int m = WideCharToMultiByte(CP_ACP, 0, tmp, -1, s, (int)n, NULL, NULL);
        if (!m) { s[n - 1] = 0; m = (int)n; }
        k = m;
    }
    free(tmp);
    return (DWORD)k;
}

typedef struct { const BLOB_ *name; DWORD type; } NameCtx;
static DWORD name_to_str_w(void *c, WCHAR *s, DWORD n) { NameCtx *x = c; return CertNameToStrW(1, x->name, x->type, s, n); }
CRYPT32API DWORD WINAPI CertNameToStrA(DWORD enc, const BLOB_ *name, DWORD type, LPSTR s, DWORD n)
{
    (void)enc;
    NameCtx c = { name, type };
    return narrow(name_to_str_w, &c, s, n);
}

/* CertGetNameString: CERT_NAME_SIMPLE_DISPLAY_TYPE and
 * CERT_NAME_FRIENDLY_DISPLAY_TYPE (CN, else OU, O, E, else the first
 * attribute), CERT_NAME_ATTR_TYPE (the attribute whose OID is @para),
 * CERT_NAME_EMAIL_TYPE, CERT_NAME_DNS_TYPE (CN) and CERT_NAME_RDN_TYPE;
 * CERT_NAME_ISSUER_FLAG for the issuer's name */
CRYPT32API DWORD WINAPI CertGetNameStringW(const void *cv, DWORD type, DWORD flags, void *para, LPWSTR s, DWORD n)
{
    const Cert *c = cv;
    Text t = { s, n, 0 };
    if (!c) return text_end(&t);
    const BLOB_ *name = flags & 1 ? &c->info.Issuer : &c->info.Subject;
    if (type == 2) return name_to_str(name, para ? *(const DWORD *)para : 3, &t);       /* RDN */
    static const char *display[] = { "2.5.4.3", "2.5.4.11", "2.5.4.10", "1.2.840.113549.1.9.1", NULL };
    static const char *email[] = { "1.2.840.113549.1.9.1", NULL };
    static const char *cn[] = { "2.5.4.3", NULL };
    const char *one[2] = { para ? para : "2.5.4.3", NULL };
    const char **want = type == 4 || type == 5 ? display : type == 1 ? email : type == 3 ? one : type == 6 ? cn : NULL;
    mbedtls_x509_name nm;
    if (want && name_parse(name->pbData, name->cbData, &nm)) {
        const mbedtls_x509_name *hit = NULL;
        for (int k = 0; want[k] && !hit; k++)
            for (const mbedtls_x509_name *x = &nm; x && !hit; x = x->next) if (x->oid.p && oid_eq(&x->oid, want[k])) hit = x;
        if (!hit && (type == 4 || type == 5) && nm.oid.p) hit = &nm;
        if (hit) put_value(&t, &hit->val, FALSE);
        name_free(&nm);
    }
    return text_end(&t);
}

typedef struct { const void *c; DWORD type, flags; void *para; } GetNameCtx;
static DWORD get_name_w(void *x, WCHAR *s, DWORD n) { GetNameCtx *g = x; return CertGetNameStringW(g->c, g->type, g->flags, g->para, s, n); }
CRYPT32API DWORD WINAPI CertGetNameStringA(const void *c, DWORD type, DWORD flags, void *para, LPSTR s, DWORD n)
{
    GetNameCtx g = { c, type, flags, para };
    return narrow(get_name_w, &g, s, n);
}
