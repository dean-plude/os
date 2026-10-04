/*
 * msg.c — signed messages (PKCS #7 / CMS SignedData) for crypt32, as
 * Authenticode uses them: CryptMsgOpenToDecode/Update/GetParam/Control,
 * the countersignature checks, and CryptQueryObject for a signed PE file,
 * a PKCS #7 blob or a certificate.
 *
 * A message is decoded from its DER encoding in place: the signer infos,
 * certificates and content are spans of the caller's bytes (copied into
 * the message).  Signatures are checked with Mbed TLS: the content's hash
 * against the signer's messageDigest attribute, then the signature over
 * the authenticated attributes (re-tagged as a SET, as PKCS #7 says) or,
 * without attributes, over the content.  For content that is not an OCTET
 * STRING (Authenticode's SpcIndirectDataContent) only the contents octets
 * are hashed, as PKCS #7 v1.5 says; CMSG_CONTENT_PARAM returns the whole
 * encoding, as Windows does.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "crypt32_int.h"

#define CRYPT32API __declspec(dllexport)

#define CRYPT_E_MSG_ERROR_          0x80091001L
#define CRYPT_E_UNKNOWN_ALGO_       0x80091002L
#define CRYPT_E_INVALID_MSG_TYPE_   0x80091004L
#define CRYPT_E_HASH_VALUE_         0x80091007L
#define CRYPT_E_INVALID_INDEX_      0x80091008L
#define CRYPT_E_NOT_DECRYPTED_      0x8009100CL
#define CRYPT_E_SIGNER_NOT_FOUND_   0x8009100EL
#define CRYPT_E_ATTRIBUTES_MISSING_ 0x8009100FL
#define CRYPT_E_STREAM_MSG_NOT_READY_ 0x80091010L
#define CRYPT_E_NO_MATCH_           0x80092009L
#define CRYPT_E_ASN1_BADTAG_        0x8009310BL
#define CRYPT_E_ASN1_EOD_           0x80093102L
#define NTE_BAD_SIGNATURE_          0x80090006L
#define NTE_BAD_ALGID_              0x80090008L

typedef struct { const BYTE *p; size_t n; } Span;

typedef struct {
    Span  raw;                  /* the SignerInfo, tag and all */
    DWORD version;
    Span  issuer, serial;       /* the Name's encoding; the INTEGER's value */
    Span  ski;                  /* [0] SubjectKeyIdentifier (version 3) */
    Span  dalg, ealg;           /* AlgorithmIdentifier encodings */
    Span  auth, unauth;         /* [0] / [1] attributes, tag and all */
    Span  sig;                  /* the encrypted digest's value */
} Signer;

typedef struct Msg {
    DWORD   magic;
    LONG    refs;
    BYTE   *data;
    size_t  n, cap;
    BOOL    decoded;
    DWORD   error;              /* why decoding failed */
    DWORD   version;
    char    inner_oid[96];
    Span    content, digested;  /* CMSG_CONTENT_PARAM; the bytes its hash covers */
    Span   *certs;
    DWORD   ncerts;
    Signer *signers;
    DWORD   nsigners;
} Msg;
#define MSG_MAGIC 0x4D534721    /* "MSG!" */

/* -----------------------------------------------------------------------
 * DER
 * ----------------------------------------------------------------------- */

/* One TLV at *p: its tag, value and whole encoding; *p moves past it */
static BOOL der_next(const BYTE **p, const BYTE *end, BYTE *tag, Span *val, Span *all)
{
    const BYTE *s = *p, *q = s;
    if (end - q < 2) return FALSE;
    BYTE t = *q++;
    size_t len = *q++;
    if (len & 0x80) {
        int k = (int)(len & 0x7F);
        if (k == 0 || k > 4 || end - q < k) return FALSE;
        len = 0;
        while (k--) len = len << 8 | *q++;
    }
    if ((size_t)(end - q) < len) return FALSE;
    if (tag) *tag = t;
    if (val) { val->p = q; val->n = len; }
    if (all) { all->p = s; all->n = (size_t)(q - s) + len; }
    *p = q + len;
    return TRUE;
}

/* The TLV at *p must have @want as its tag */
static BOOL der_expect(const BYTE **p, const BYTE *end, BYTE want, Span *val, Span *all)
{
    BYTE t;
    const BYTE *save = *p;
    if (!der_next(p, end, &t, val, all) || t != want) { *p = save; return FALSE; }
    return TRUE;
}

static void oid_string(Span oid, char *out, size_t cap)
{
    mbedtls_asn1_buf b = { 0x06, oid.n, (unsigned char *)oid.p };
    if (mbedtls_oid_get_numeric_string(out, cap, &b) < 0) out[0] = 0;
}

static BOOL oid_is(Span oid, const char *text)
{
    char s[96];
    oid_string(oid, s, sizeof(s));
    return !strcmp(s, text);
}

/* AlgorithmIdentifier: its OID and parameters (an encoding, or none) */
static BOOL alg_parts(Span alg, Span *oid, Span *params)
{
    const BYTE *p = alg.p, *end = alg.p + alg.n;
    Span seq;
    if (!der_expect(&p, end, 0x30, &seq, NULL)) return FALSE;
    p = seq.p;
    end = seq.p + seq.n;
    if (!der_expect(&p, end, 0x06, oid, NULL)) return FALSE;
    params->p = p;
    params->n = (size_t)(end - p);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Decoding
 * ----------------------------------------------------------------------- */
static BOOL parse_signer(Span raw, Signer *s)
{
    memset(s, 0, sizeof(*s));
    s->raw = raw;
    const BYTE *p = raw.p, *end = raw.p + raw.n;
    Span seq, v, all;
    BYTE t;
    if (!der_expect(&p, end, 0x30, &seq, NULL)) return FALSE;
    p = seq.p;
    end = seq.p + seq.n;
    if (!der_expect(&p, end, 0x02, &v, NULL) || v.n > 4) return FALSE;
    for (size_t i = 0; i < v.n; i++) s->version = s->version << 8 | v.p[i];
    if (!der_next(&p, end, &t, &v, &all)) return FALSE;
    if (t == 0x30) {                                    /* IssuerAndSerialNumber */
        const BYTE *q = v.p, *qe = v.p + v.n;
        if (!der_expect(&q, qe, 0x30, NULL, &s->issuer) || !der_expect(&q, qe, 0x02, &s->serial, NULL)) return FALSE;
    } else if (t == 0x80) s->ski = v;                   /* [0] SubjectKeyIdentifier */
    else return FALSE;
    if (!der_expect(&p, end, 0x30, NULL, &s->dalg)) return FALSE;
    der_expect(&p, end, 0xA0, NULL, &s->auth);
    if (!der_expect(&p, end, 0x30, NULL, &s->ealg)) return FALSE;
    if (!der_expect(&p, end, 0x04, &s->sig, NULL)) return FALSE;
    der_expect(&p, end, 0xA1, NULL, &s->unauth);
    return TRUE;
}

static DWORD decode(Msg *m)
{
    const BYTE *p = m->data, *end = m->data + m->n;
    Span ci, v, oid;
    if (!der_expect(&p, end, 0x30, &ci, NULL)) return CRYPT_E_ASN1_BADTAG_;
    p = ci.p;
    end = ci.p + ci.n;
    if (!der_expect(&p, end, 0x06, &oid, NULL)) return CRYPT_E_ASN1_BADTAG_;
    if (!oid_is(oid, "1.2.840.113549.1.7.2")) return CRYPT_E_INVALID_MSG_TYPE_;   /* signedData only */
    if (!der_expect(&p, end, 0xA0, &v, NULL)) return CRYPT_E_ASN1_BADTAG_;
    p = v.p;
    end = v.p + v.n;
    Span sd;
    if (!der_expect(&p, end, 0x30, &sd, NULL)) return CRYPT_E_ASN1_BADTAG_;
    p = sd.p;
    end = sd.p + sd.n;
    if (!der_expect(&p, end, 0x02, &v, NULL) || v.n > 4) return CRYPT_E_ASN1_BADTAG_;
    for (size_t i = 0; i < v.n; i++) m->version = m->version << 8 | v.p[i];
    if (!der_expect(&p, end, 0x31, NULL, NULL)) return CRYPT_E_ASN1_BADTAG_;    /* digestAlgorithms */
    /* contentInfo: the inner type and, unless detached, the content */
    Span inner;
    if (!der_expect(&p, end, 0x30, &inner, NULL)) return CRYPT_E_ASN1_BADTAG_;
    {
        const BYTE *q = inner.p, *qe = inner.p + inner.n;
        Span ioid, wrap;
        if (!der_expect(&q, qe, 0x06, &ioid, NULL)) return CRYPT_E_ASN1_BADTAG_;
        oid_string(ioid, m->inner_oid, sizeof(m->inner_oid));
        if (der_expect(&q, qe, 0xA0, &wrap, NULL)) {
            const BYTE *r = wrap.p;
            BYTE t;
            Span val, all;
            if (!der_next(&r, wrap.p + wrap.n, &t, &val, &all)) return CRYPT_E_ASN1_BADTAG_;
            if (t == 0x04) { m->content = val; m->digested = val; }    /* data, or CMS eContent */
            else { m->content = all; m->digested = val; }              /* PKCS #7: the contents octets */
        }
    }
    /* [0] certificates, [1] CRLs */
    Span certs;
    if (der_expect(&p, end, 0xA0, &certs, NULL)) {
        DWORD cap = 0;
        for (const BYTE *q = certs.p; q < certs.p + certs.n;) {
            BYTE t;
            Span all;
            if (!der_next(&q, certs.p + certs.n, &t, NULL, &all)) return CRYPT_E_ASN1_BADTAG_;
            if (t != 0x30) continue;                                    /* (other certificate formats) */
            if (m->ncerts == cap) {
                cap = cap ? cap * 2 : 8;
                Span *n2 = realloc(m->certs, cap * sizeof(Span));
                if (!n2) return ERROR_NOT_ENOUGH_MEMORY;
                m->certs = n2;
            }
            m->certs[m->ncerts++] = all;
        }
    }
    der_expect(&p, end, 0xA1, NULL, NULL);
    Span sis;
    if (!der_expect(&p, end, 0x31, &sis, NULL)) return CRYPT_E_ASN1_BADTAG_;
    DWORD cap = 0;
    for (const BYTE *q = sis.p; q < sis.p + sis.n;) {
        Span all;
        if (!der_next(&q, sis.p + sis.n, NULL, NULL, &all)) return CRYPT_E_ASN1_BADTAG_;
        if (m->nsigners == cap) {
            cap = cap ? cap * 2 : 2;
            Signer *n2 = realloc(m->signers, cap * sizeof(Signer));
            if (!n2) return ERROR_NOT_ENOUGH_MEMORY;
            m->signers = n2;
        }
        if (!parse_signer(all, &m->signers[m->nsigners])) return CRYPT_E_ASN1_BADTAG_;
        m->nsigners++;
    }
    return 0;
}

static Msg *msg_of(HANDLE h)
{
    Msg *m = h;
    if (!m || m->magic != MSG_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return m;
}

/* The message, decoded (on the first call after its last update) */
static Msg *decoded(HANDLE h)
{
    Msg *m = msg_of(h);
    if (!m) return NULL;
    if (!m->decoded) {
        m->error = m->n ? decode(m) : CRYPT_E_STREAM_MSG_NOT_READY_;
        m->decoded = TRUE;
    }
    if (m->error) { SetLastError(m->error); return NULL; }
    return m;
}

CRYPT32API HANDLE WINAPI CryptMsgOpenToDecode(DWORD enc, DWORD flags, DWORD type, HANDLE prov, void *recip, const void *stream)
{
    (void)enc; (void)flags; (void)prov; (void)recip;
    if (stream) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }          /* (streamed decoding) */
    if (type && type != 2 /* CMSG_SIGNED */) { SetLastError(CRYPT_E_INVALID_MSG_TYPE_); return 0; }
    Msg *m = calloc(1, sizeof(*m));
    if (!m) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    m->magic = MSG_MAGIC;
    m->refs = 1;
    return m;
}

CRYPT32API BOOL WINAPI CryptMsgUpdate(HANDLE h, const BYTE *data, DWORD n, BOOL final)
{
    Msg *m = msg_of(h);
    if (!m) return FALSE;
    if (m->decoded && !m->error) { SetLastError(CRYPT_E_MSG_ERROR_); return FALSE; }
    if (n) {
        if (m->n + n > m->cap) {
            size_t cap = (m->n + n) * 2;
            BYTE *d = realloc(m->data, cap);
            if (!d) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
            m->data = d;
            m->cap = cap;
        }
        memcpy(m->data + m->n, data, n);
        m->n += n;
        m->decoded = FALSE;
    }
    if (!final) return TRUE;
    return decoded(h) != NULL;
}

CRYPT32API HANDLE WINAPI CryptMsgDuplicate(HANDLE h)
{
    Msg *m = msg_of(h);
    if (m) InterlockedIncrement(&m->refs);
    return m;
}

CRYPT32API BOOL WINAPI CryptMsgClose(HANDLE h)
{
    Msg *m = h;
    if (!m) return TRUE;
    if (m->magic != MSG_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (InterlockedDecrement(&m->refs)) return TRUE;
    m->magic = 0;
    free(m->data);
    free(m->certs);
    free(m->signers);
    free(m);
    return TRUE;
}

HANDLE crypt32_msg_from_blob(const BYTE *b, DWORD n)
{
    HANDLE m = CryptMsgOpenToDecode(0x10001, 0, 0, 0, NULL, NULL);
    if (m && !CryptMsgUpdate(m, b, n, TRUE)) {
        DWORD e = GetLastError();
        CryptMsgClose(m);
        SetLastError(e);
        return 0;
    }
    return m;
}

DWORD crypt32_msg_cert_count(HANDLE h) { Msg *m = decoded(h); return m ? m->ncerts : 0; }

const BYTE *crypt32_msg_cert(HANDLE h, DWORD i, DWORD *n)
{
    Msg *m = decoded(h);
    if (!m || i >= m->ncerts) return NULL;
    *n = (DWORD)m->certs[i].n;
    return m->certs[i].p;
}

/* -----------------------------------------------------------------------
 * CryptMsgGetParam: values copied out, structures laid out in the caller's
 * buffer (the structure first, then what its pointers point to)
 * ----------------------------------------------------------------------- */
typedef struct { BYTE *buf; size_t cap, off; } Pack;

static void *pk_take(Pack *k, size_t n)
{
    size_t at = (k->off + 7) & ~(size_t)7;
    k->off = at + n;
    return k->buf && k->off <= k->cap ? k->buf + at : NULL;
}

static BYTE *pk_bytes(Pack *k, const void *src, size_t n)
{
    BYTE *d = pk_take(k, n ? n : 1);
    if (d && n) memcpy(d, src, n);
    return d;
}

static char *pk_oid(Pack *k, Span oid)
{
    char s[96];
    oid_string(oid, s, sizeof(s));
    return (char *)pk_bytes(k, s, strlen(s) + 1);
}

typedef struct { LPSTR pszObjId; BLOB_ Parameters; } ALGID_;
typedef struct { LPSTR pszObjId; DWORD cValue; BLOB_ *rgValue; } ATTR_;
typedef struct { DWORD cAttr; ATTR_ *rgAttr; } ATTRS_;
typedef struct {
    DWORD dwVersion;
    BLOB_ Issuer;
    BLOB_ SerialNumber;
    ALGID_ HashAlgorithm;
    ALGID_ HashEncryptionAlgorithm;
    BLOB_ EncryptedHash;
    ATTRS_ AuthAttrs;
    ATTRS_ UnauthAttrs;
} SIGNER_INFO_;

static void pk_alg(Pack *k, Span alg, ALGID_ *out)
{
    Span oid, params;
    if (!alg_parts(alg, &oid, &params)) return;
    char *s = pk_oid(k, oid);
    BYTE *pp = params.n ? pk_bytes(k, params.p, params.n) : NULL;
    if (out) { out->pszObjId = s; out->Parameters.cbData = (DWORD)params.n; out->Parameters.pbData = pp; }
}

/* The serial number little-endian, as Windows keeps it */
static void pk_serial(Pack *k, Span serial, BLOB_ *out)
{
    BYTE *d = pk_take(k, serial.n ? serial.n : 1);
    if (d) for (size_t i = 0; i < serial.n; i++) d[i] = serial.p[serial.n - 1 - i];
    if (out) { out->cbData = (DWORD)serial.n; out->pbData = d; }
}

/* [0]/[1] SET OF Attribute { OID, SET OF value } */
static void pk_attrs(Pack *k, Span tagged, ATTRS_ *out)
{
    const BYTE *p = tagged.p, *end = tagged.p + tagged.n;
    Span set;
    if (out) { out->cAttr = 0; out->rgAttr = NULL; }
    if (!tagged.n || !der_next(&p, end, NULL, &set, NULL)) return;
    DWORD n = 0;
    for (const BYTE *q = set.p; q < set.p + set.n; n++) if (!der_next(&q, set.p + set.n, NULL, NULL, NULL)) break;
    ATTR_ *arr = pk_take(k, n * sizeof(ATTR_) + 1);
    DWORD i = 0;
    for (const BYTE *q = set.p; q < set.p + set.n && i < n; i++) {
        Span attr, oid, vals;
        if (!der_next(&q, set.p + set.n, NULL, &attr, NULL)) break;
        const BYTE *r = attr.p, *re = attr.p + attr.n;
        if (!der_expect(&r, re, 0x06, &oid, NULL) || !der_expect(&r, re, 0x31, &vals, NULL)) continue;
        char *s = pk_oid(k, oid);
        DWORD nv = 0;
        for (const BYTE *v = vals.p; v < vals.p + vals.n; nv++) if (!der_next(&v, vals.p + vals.n, NULL, NULL, NULL)) break;
        BLOB_ *blobs = pk_take(k, nv * sizeof(BLOB_) + 1);
        DWORD j = 0;
        for (const BYTE *v = vals.p; v < vals.p + vals.n && j < nv; j++) {
            Span all;
            if (!der_next(&v, vals.p + vals.n, NULL, NULL, &all)) break;
            BYTE *d = pk_bytes(k, all.p, all.n);
            if (blobs) { blobs[j].cbData = (DWORD)all.n; blobs[j].pbData = d; }
        }
        if (arr) { arr[i].pszObjId = s; arr[i].cValue = nv; arr[i].rgValue = blobs; }
    }
    if (out) { out->cAttr = n; out->rgAttr = arr; }
}

static void pack_signer(Pack *k, const Signer *s)
{
    SIGNER_INFO_ *si = pk_take(k, sizeof(*si));
    if (si) memset(si, 0, sizeof(*si));
    BYTE *iss = pk_bytes(k, s->issuer.p, s->issuer.n);
    if (si) { si->dwVersion = s->version; si->Issuer.cbData = (DWORD)s->issuer.n; si->Issuer.pbData = iss; }
    pk_serial(k, s->serial, si ? &si->SerialNumber : NULL);
    pk_alg(k, s->dalg, si ? &si->HashAlgorithm : NULL);
    pk_alg(k, s->ealg, si ? &si->HashEncryptionAlgorithm : NULL);
    BYTE *sig = pk_bytes(k, s->sig.p, s->sig.n);
    if (si) { si->EncryptedHash.cbData = (DWORD)s->sig.n; si->EncryptedHash.pbData = sig; }
    pk_attrs(k, s->auth, si ? &si->AuthAttrs : NULL);
    pk_attrs(k, s->unauth, si ? &si->UnauthAttrs : NULL);
}

static void pack_cert_info(Pack *k, const Signer *s)
{
    CERT_INFO_ *ci = pk_take(k, sizeof(*ci));
    if (ci) memset(ci, 0, sizeof(*ci));
    BYTE *iss = pk_bytes(k, s->issuer.p, s->issuer.n);
    if (ci) { ci->Issuer.cbData = (DWORD)s->issuer.n; ci->Issuer.pbData = iss; }
    pk_serial(k, s->serial, ci ? &ci->SerialNumber : NULL);
}

/* Lay out @what (twice: to size it, then into @data) */
typedef void (*Packer)(Pack *, const Signer *);
static BOOL out_packed(Packer f, const Signer *s, void *data, DWORD *n)
{
    Pack k = { NULL, 0, 0 };
    f(&k, s);
    DWORD need = (DWORD)k.off;
    if (!data) { *n = need; return TRUE; }
    if (*n < need) { *n = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    Pack w = { data, *n, 0 };
    f(&w, s);
    *n = need;
    return TRUE;
}

static void pack_hash_alg(Pack *k, const Signer *s)
{
    ALGID_ *a = pk_take(k, sizeof(*a));
    pk_alg(k, s->dalg, a);
}
static void pack_auth(Pack *k, const Signer *s)
{
    ATTRS_ *a = pk_take(k, sizeof(*a));
    pk_attrs(k, s->auth, a);
}
static void pack_unauth(Pack *k, const Signer *s)
{
    ATTRS_ *a = pk_take(k, sizeof(*a));
    pk_attrs(k, s->unauth, a);
}

static BOOL out_bytes(const void *src, size_t len, void *data, DWORD *n)
{
    if (!data) { *n = (DWORD)len; return TRUE; }
    if (*n < len) { *n = (DWORD)len; SetLastError(ERROR_MORE_DATA); return FALSE; }
    memcpy(data, src, len);
    *n = (DWORD)len;
    return TRUE;
}

static BOOL out_dword(DWORD v, void *data, DWORD *n) { return out_bytes(&v, sizeof(v), data, n); }

CRYPT32API BOOL WINAPI CryptMsgGetParam(HANDLE h, DWORD type, DWORD index, void *data, DWORD *n)
{
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Msg *m = decoded(h);
    if (!m) return FALSE;
    const Signer *s = index < m->nsigners ? &m->signers[index] : NULL;
    switch (type) {
    case 1:  return out_dword(2 /* CMSG_SIGNED */, data, n);                 /* CMSG_TYPE_PARAM */
    case 2:  return out_bytes(m->content.p, m->content.n, data, n);          /* CMSG_CONTENT_PARAM */
    case 4:  return out_bytes(m->inner_oid, strlen(m->inner_oid) + 1, data, n);  /* CMSG_INNER_CONTENT_TYPE_PARAM */
    case 5:  return out_dword(m->nsigners, data, n);                         /* CMSG_SIGNER_COUNT_PARAM */
    case 11: return out_dword(m->ncerts, data, n);                           /* CMSG_CERT_COUNT_PARAM */
    case 12:                                                                 /* CMSG_CERT_PARAM */
        if (index >= m->ncerts) { SetLastError(CRYPT_E_INVALID_INDEX_); return FALSE; }
        return out_bytes(m->certs[index].p, m->certs[index].n, data, n);
    case 13: return out_dword(0, data, n);                                   /* CMSG_CRL_COUNT_PARAM */
    case 29: return out_bytes(m->data, m->n, data, n);                       /* CMSG_ENCODED_MESSAGE */
    case 30: return out_dword(m->version, data, n);                          /* CMSG_VERSION_PARAM */
    }
    if (!s) { SetLastError(CRYPT_E_INVALID_INDEX_); return FALSE; }
    switch (type) {
    case 6:  return out_packed(pack_signer, s, data, n);                     /* CMSG_SIGNER_INFO_PARAM */
    case 7:  return out_packed(pack_cert_info, s, data, n);                  /* CMSG_SIGNER_CERT_INFO_PARAM */
    case 8:  return out_packed(pack_hash_alg, s, data, n);                   /* CMSG_SIGNER_HASH_ALGORITHM_PARAM */
    case 9:                                                                  /* CMSG_SIGNER_AUTH_ATTR_PARAM */
        if (!s->auth.n) { SetLastError(CRYPT_E_ATTRIBUTES_MISSING_); return FALSE; }
        return out_packed(pack_auth, s, data, n);
    case 10:                                                                 /* CMSG_SIGNER_UNAUTH_ATTR_PARAM */
        if (!s->unauth.n) { SetLastError(CRYPT_E_ATTRIBUTES_MISSING_); return FALSE; }
        return out_packed(pack_unauth, s, data, n);
    case 28: return out_bytes(s->raw.p, s->raw.n, data, n);                  /* CMSG_ENCODED_SIGNER */
    }
    SetLastError(CRYPT_E_INVALID_MSG_TYPE_);
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Signatures
 * ----------------------------------------------------------------------- */

/* The signer's public key from a certificate's encoding */
static BOOL key_from_cert(const BYTE *der, size_t n, mbedtls_x509_crt *crt)
{
    mbedtls_x509_crt_init(crt);
    return mbedtls_x509_crt_parse_der(crt, der, n) == 0;
}

/* ... or from a CERT_PUBLIC_KEY_INFO, encoded back into SubjectPublicKeyInfo */
static size_t der_len(BYTE *out, size_t n)
{
    if (n < 0x80) { if (out) out[0] = (BYTE)n; return 1; }
    int k = n > 0xFFFFFF ? 4 : n > 0xFFFF ? 3 : n > 0xFF ? 2 : 1;
    if (out) { out[0] = (BYTE)(0x80 | k); for (int i = 0; i < k; i++) out[1 + i] = (BYTE)(n >> (8 * (k - 1 - i))); }
    return 1 + (size_t)k;
}

static BOOL key_from_info(const PUBKEY_INFO_ *ki, mbedtls_pk_context *pk)
{
    mbedtls_asn1_buf oid = { 0 };
    if (!ki || !ki->Algorithm.pszObjId ||
        mbedtls_oid_from_numeric_string(&oid, ki->Algorithm.pszObjId, strlen(ki->Algorithm.pszObjId)))
        return FALSE;
    size_t alg_body = 2 + oid.len + ki->Algorithm.Parameters.cbData;   /* (OIDs are short) */
    size_t bits_body = 1 + ki->PublicKey.cbData;
    size_t body = 1 + der_len(NULL, alg_body) + alg_body + 1 + der_len(NULL, bits_body) + bits_body;
    size_t total = 1 + der_len(NULL, body) + body;
    BYTE *b = malloc(total), *q = b;
    BOOL ok = FALSE;
    if (b) {
        *q++ = 0x30; q += der_len(q, body);
        *q++ = 0x30; q += der_len(q, alg_body);
        *q++ = 0x06; *q++ = (BYTE)oid.len; memcpy(q, oid.p, oid.len); q += oid.len;
        if (ki->Algorithm.Parameters.cbData) { memcpy(q, ki->Algorithm.Parameters.pbData, ki->Algorithm.Parameters.cbData); q += ki->Algorithm.Parameters.cbData; }
        *q++ = 0x03; q += der_len(q, bits_body);
        *q++ = (BYTE)ki->PublicKey.cUnusedBits;
        memcpy(q, ki->PublicKey.pbData, ki->PublicKey.cbData);
        mbedtls_pk_init(pk);
        ok = mbedtls_pk_parse_public_key(pk, b, total) == 0;
        free(b);
    }
    free(oid.p);
    return ok;
}

/* The digest algorithm of an AlgorithmIdentifier (a hash, or the hash of
 * a signature algorithm, which some signers write there) */
static mbedtls_md_type_t md_of(Span alg)
{
    Span oid, params;
    if (!alg_parts(alg, &oid, &params)) return MBEDTLS_MD_NONE;
    mbedtls_asn1_buf b = { 0x06, oid.n, (unsigned char *)oid.p };
    mbedtls_md_type_t md = MBEDTLS_MD_NONE;
    mbedtls_pk_type_t pkt;
    if (mbedtls_oid_get_md_alg(&b, &md) && mbedtls_oid_get_sig_alg(&b, &md, &pkt)) return MBEDTLS_MD_NONE;
    return md;
}

/* The value of the first attribute @oid in [0]/[1] attributes */
static BOOL find_attr(Span tagged, const char *oid_text, Span *value)
{
    const BYTE *p = tagged.p, *end = tagged.p + tagged.n;
    Span set;
    if (!tagged.n || !der_next(&p, end, NULL, &set, NULL)) return FALSE;
    for (const BYTE *q = set.p; q < set.p + set.n;) {
        Span attr, oid, vals;
        if (!der_next(&q, set.p + set.n, NULL, &attr, NULL)) return FALSE;
        const BYTE *r = attr.p, *re = attr.p + attr.n;
        if (!der_expect(&r, re, 0x06, &oid, NULL) || !der_expect(&r, re, 0x31, &vals, NULL)) continue;
        if (!oid_is(oid, oid_text)) continue;
        const BYTE *v = vals.p;
        return der_next(&v, vals.p + vals.n, NULL, NULL, value);
    }
    return FALSE;
}

/* Signer @s's signature over @content with @key: 0 or the error */
static DWORD verify_signer(const Signer *s, const BYTE *content, size_t clen, mbedtls_pk_context *key)
{
    mbedtls_md_type_t md = md_of(s->dalg);
    const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(md);
    if (!mi) return NTE_BAD_ALGID_;
    Span eoid, eparams;
    if (alg_parts(s->ealg, &eoid, &eparams) && oid_is(eoid, "1.2.840.113549.1.1.10")) return NTE_BAD_ALGID_;  /* (RSA-PSS) */
    BYTE hash[MBEDTLS_MD_MAX_SIZE];
    size_t hl = mbedtls_md_get_size(mi);
    if (mbedtls_md(mi, content, clen, hash)) return NTE_BAD_ALGID_;
    if (s->auth.n) {
        Span dig, dv;
        if (!find_attr(s->auth, "1.2.840.113549.1.9.4", &dig)) return CRYPT_E_ATTRIBUTES_MISSING_;  /* messageDigest */
        const BYTE *p = dig.p;
        if (!der_expect(&p, dig.p + dig.n, 0x04, &dv, NULL) || dv.n != hl || memcmp(dv.p, hash, hl)) return CRYPT_E_HASH_VALUE_;
        /* the signature covers the attributes re-tagged as SET OF */
        mbedtls_md_context_t c;
        static const BYTE set_tag = 0x31;
        mbedtls_md_init(&c);
        int bad = mbedtls_md_setup(&c, mi, 0) || mbedtls_md_starts(&c) || mbedtls_md_update(&c, &set_tag, 1) ||
                  mbedtls_md_update(&c, s->auth.p + 1, s->auth.n - 1) || mbedtls_md_finish(&c, hash);
        mbedtls_md_free(&c);
        if (bad) return NTE_BAD_ALGID_;
    }
    return mbedtls_pk_verify(key, md, hash, hl, s->sig.p, s->sig.n) ? NTE_BAD_SIGNATURE_ : 0;
}

/* Whether @s names the certificate with @issuer and @serial (little-endian) */
static BOOL names_cert(const Signer *s, const BLOB_ *issuer, const BLOB_ *serial)
{
    if (!issuer || !serial || issuer->cbData != s->issuer.n || memcmp(issuer->pbData, s->issuer.p, s->issuer.n)) return FALSE;
    /* compare as integers: leading zero bytes do not count */
    size_t a = s->serial.n, b = serial->cbData, ia = 0;
    while (a > 1 && s->serial.p[ia] == 0) { ia++; a--; }
    while (b > 1 && serial->pbData[b - 1] == 0) b--;
    if (a != b) return FALSE;
    for (size_t i = 0; i < a; i++) if (s->serial.p[ia + i] != serial->pbData[b - 1 - i]) return FALSE;
    return TRUE;
}

static DWORD verify_with_info(const Signer *s, const BYTE *content, size_t clen, const PUBKEY_INFO_ *ki)
{
    mbedtls_pk_context pk;
    if (!key_from_info(ki, &pk)) { mbedtls_pk_free(&pk); return NTE_BAD_ALGID_; }
    DWORD e = verify_signer(s, content, clen, &pk);
    mbedtls_pk_free(&pk);
    return e;
}

static DWORD verify_with_cert(const Signer *s, const BYTE *content, size_t clen, const CERT_CONTEXT_ *cert)
{
    mbedtls_x509_crt crt;
    DWORD e = cert && key_from_cert(cert->pbCertEncoded, cert->cbCertEncoded, &crt) ?
              verify_signer(s, content, clen, &crt.pk) : NTE_BAD_ALGID_;
    mbedtls_x509_crt_free(&crt);
    return e;
}

typedef struct { DWORD cbSize; ULONG_PTR hCryptProv; DWORD dwSignerIndex, dwSignerType; void *pvSigner; } VERIFY_EX_PARA_;

CRYPT32API BOOL WINAPI CryptMsgControl(HANDLE h, DWORD flags, DWORD type, const void *para)
{
    (void)flags;
    Msg *m = decoded(h);
    if (!m) return FALSE;
    DWORD e = CRYPT_E_SIGNER_NOT_FOUND_;
    if (type == 1) {                                    /* CMSG_CTRL_VERIFY_SIGNATURE: a CERT_INFO */
        const CERT_INFO_ *ci = para;
        if (!ci) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        for (DWORD i = 0; i < m->nsigners; i++)
            if (names_cert(&m->signers[i], &ci->Issuer, &ci->SerialNumber)) {
                e = verify_with_info(&m->signers[i], m->digested.p, m->digested.n, &ci->SubjectPublicKeyInfo);
                break;
            }
    } else if (type == 19) {                            /* CMSG_CTRL_VERIFY_SIGNATURE_EX */
        const VERIFY_EX_PARA_ *v = para;
        if (!v || v->dwSignerIndex >= m->nsigners) { SetLastError(v ? CRYPT_E_SIGNER_NOT_FOUND_ : ERROR_INVALID_PARAMETER); return FALSE; }
        const Signer *s = &m->signers[v->dwSignerIndex];
        if (v->dwSignerType == 1) e = verify_with_info(s, m->digested.p, m->digested.n, v->pvSigner);      /* PUBKEY */
        else if (v->dwSignerType == 2) e = verify_with_cert(s, m->digested.p, m->digested.n, v->pvSigner); /* CERT */
        else e = ERROR_NOT_SUPPORTED;
    } else e = ERROR_NOT_SUPPORTED;
    if (e) { SetLastError(e); return FALSE; }
    return TRUE;
}

/* A countersignature: @cs signs the encrypted digest of the signer @si */
static BOOL countersig(const BYTE *si, DWORD nsi, const BYTE *cs, DWORD ncs, DWORD kind, const void *signer)
{
    Signer outer, counter;
    if (!si || !cs || !parse_signer((Span){ si, nsi }, &outer) || !parse_signer((Span){ cs, ncs }, &counter)) {
        SetLastError(CRYPT_E_ASN1_BADTAG_);
        return FALSE;
    }
    DWORD e = kind == 1 ? verify_with_info(&counter, outer.sig.p, outer.sig.n, signer) :
              kind == 2 ? verify_with_cert(&counter, outer.sig.p, outer.sig.n, signer) : ERROR_NOT_SUPPORTED;
    if (e) { SetLastError(e); return FALSE; }
    return TRUE;
}

CRYPT32API BOOL WINAPI CryptMsgVerifyCountersignatureEncoded(ULONG_PTR prov, DWORD enc, const BYTE *si, DWORD nsi,
                                                              const BYTE *cs, DWORD ncs, CERT_INFO_ *signer)
{
    (void)prov; (void)enc;
    if (!signer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Signer counter;
    if (cs && parse_signer((Span){ cs, ncs }, &counter) && !names_cert(&counter, &signer->Issuer, &signer->SerialNumber)) {
        SetLastError(CRYPT_E_SIGNER_NOT_FOUND_);
        return FALSE;
    }
    return countersig(si, nsi, cs, ncs, 1, &signer->SubjectPublicKeyInfo);
}

CRYPT32API BOOL WINAPI CryptMsgVerifyCountersignatureEncodedEx(ULONG_PTR prov, DWORD enc, const BYTE *si, DWORD nsi,
                                                                const BYTE *cs, DWORD ncs, DWORD kind, void *signer,
                                                                DWORD flags, void *extra)
{
    (void)prov; (void)enc; (void)flags; (void)extra;
    return countersig(si, nsi, cs, ncs, kind, signer);
}

/* -----------------------------------------------------------------------
 * CryptQueryObject
 * ----------------------------------------------------------------------- */

/* The first PKCS #7 signature in a PE file's certificate table, allocated */
static BYTE *pe_signature(const BYTE *(*at)(void *, DWORD, DWORD), void *ctx, DWORD *len)
{
    const BYTE *mz = at(ctx, 0, 64);
    if (!mz || mz[0] != 'M' || mz[1] != 'Z') return NULL;
    DWORD pe = *(const DWORD *)(mz + 60);
    const BYTE *nt = at(ctx, pe, 24 + 2);
    if (!nt || memcmp(nt, "PE\0\0", 4)) return NULL;
    WORD magic = *(const WORD *)(nt + 24);
    DWORD dir = pe + 24 + (magic == 0x20B ? 112 : 96) + 4 * 8;
    const BYTE *d = at(ctx, dir, 8);
    if (!d) return NULL;
    DWORD off = *(const DWORD *)d, size = *(const DWORD *)(d + 4);
    for (DWORD pos = off; size && pos + 8 <= off + size;) {
        const BYTE *wc = at(ctx, pos, 8);
        if (!wc) return NULL;
        DWORD n = *(const DWORD *)wc;
        WORD kind = *(const WORD *)(wc + 6);
        if (n < 8 || n > size) return NULL;
        if (kind == 2) {                                /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */
            const BYTE *b = at(ctx, pos + 8, n - 8);
            BYTE *copy = b ? malloc(n - 8) : NULL;
            if (copy) { memcpy(copy, b, n - 8); *len = n - 8; }
            return copy;
        }
        pos += (n + 7) & ~7u;
    }
    return NULL;
}

typedef struct { HANDLE f; BYTE *buf; DWORD cap; const BYTE *blob; DWORD n; } Src;

static const BYTE *src_at(void *p, DWORD off, DWORD n)
{
    Src *s = p;
    if (s->blob) return (ULONGLONG)off + n <= s->n ? s->blob + off : NULL;
    if (n > s->cap) {
        BYTE *b = realloc(s->buf, n);
        if (!b) return NULL;
        s->buf = b;
        s->cap = n;
    }
    LARGE_INTEGER li;
    li.QuadPart = off;
    DWORD got = 0;
    if (!SetFilePointerEx(s->f, li, NULL, FILE_BEGIN) || !ReadFile(s->f, s->buf, n, &got, NULL) || got != n) return NULL;
    return s->buf;
}

typedef struct { DWORD cbData; BYTE *pbData; } DATA_BLOB_;

CRYPT32API BOOL WINAPI CryptQueryObject(DWORD type, const void *obj, DWORD ct, DWORD ft, DWORD flags, DWORD *enc,
                                        DWORD *ctype, DWORD *ftype, HANDLE *store, HANDLE *msg, const void **ctx)
{
    (void)ft; (void)flags;
    if (store) *store = 0;
    if (msg) *msg = 0;
    if (ctx) *ctx = 0;
    Src src = { INVALID_HANDLE_VALUE, NULL, 0, NULL, 0 };
    BYTE *whole = NULL;
    if (type == 1) {                                    /* CERT_QUERY_OBJECT_FILE */
        src.f = CreateFileW(obj, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (src.f == INVALID_HANDLE_VALUE) return FALSE;
    } else if (type == 2 && obj) {                      /* CERT_QUERY_OBJECT_BLOB */
        const DATA_BLOB_ *b = obj;
        src.blob = b->pbData;
        src.n = b->cbData;
    } else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    DWORD kind = 0, len = 0;
    BYTE *sig = (ct & (1u << 10)) ? pe_signature(src_at, &src, &len) : NULL;   /* PKCS7_SIGNED_EMBED */
    if (sig) kind = 10;
    else {
        /* the object itself: a PKCS #7 message or a certificate */
        if (src.f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(src.f, &sz) && sz.QuadPart > 0 && sz.QuadPart < 64 << 20) {
                DWORD got = 0;
                LARGE_INTEGER zero = { 0 };
                whole = malloc((size_t)sz.QuadPart);
                if (whole && SetFilePointerEx(src.f, zero, NULL, FILE_BEGIN) &&
                    ReadFile(src.f, whole, (DWORD)sz.QuadPart, &got, NULL) && got == sz.QuadPart) {
                    src.blob = whole;
                    src.n = got;
                }
            }
        }
        HANDLE m = 0;
        const void *cert = NULL;
        if (src.blob && (ct & (1u << 8)) && (m = crypt32_msg_from_blob(src.blob, src.n)) != 0) kind = 8;      /* PKCS7_SIGNED */
        else if (src.blob && (ct & (1u << 1)) && (cert = CertCreateCertificateContext(1, src.blob, src.n)) != NULL) kind = 1;  /* CERT */
        if (kind == 8) {
            if (enc) *enc = 0x10001;
            if (ctype) *ctype = kind;
            if (ftype) *ftype = 1;
            if (store) *store = CertOpenStore((LPCSTR)1, 0x10001, 0, 0, m);
            if (msg) *msg = m; else CryptMsgClose(m);
        } else if (kind == 1) {
            if (enc) *enc = 1;
            if (ctype) *ctype = kind;
            if (ftype) *ftype = 1;
            if (store) { *store = crypt32_memory_store(); CertAddCertificateContextToStore(*store, cert, 4, NULL); }
            if (ctx) *ctx = cert; else CertFreeCertificateContext(cert);
        }
    }
    if (sig) {
        HANDLE m = crypt32_msg_from_blob(sig, len);
        if (!m) kind = 0;
        else {
            if (enc) *enc = 0x10001;
            if (ctype) *ctype = kind;
            if (ftype) *ftype = 1;
            if (store) *store = CertOpenStore((LPCSTR)1, 0x10001, 0, 0, m);
            if (msg) *msg = m; else CryptMsgClose(m);
        }
        free(sig);
    }
    if (src.f != INVALID_HANDLE_VALUE) CloseHandle(src.f);
    free(src.buf);
    free(whole);
    if (!kind) { SetLastError(CRYPT_E_NO_MATCH_); return FALSE; }
    return TRUE;
}
