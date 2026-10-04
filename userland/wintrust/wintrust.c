/*
 * wintrust.dll — Authenticode.  WinVerifyTrust with the generic verify
 * action (WINTRUST_ACTION_GENERIC_VERIFY_V2) checks the signature a PE
 * file carries in its certificate table, as Windows' software publisher
 * provider does:
 *
 *   1. the file's Authenticode digest (the whole file but its checksum,
 *      the certificate table's directory entry and the table itself, with
 *      the algorithm the signature names) against the digest in the signed
 *      SpcIndirectDataContent;
 *   2. the signer's signature over that content (crypt32's CryptMsgControl);
 *   3. a timestamp, if there is one (a PKCS #9 countersignature or an
 *      RFC 3161 token), which fixes the time the chain is checked at;
 *   4. the signer's certificate chain to a trusted root with crypt32's
 *      CertGetCertificateChain (code signing usage) and the Authenticode
 *      chain policy.
 *
 * Revocation lists are never fetched: a check that asks for revocation
 * fails as on a Windows machine that is offline (CERT_E_REVOCATION_FAILURE).
 * Nested (secondary) signatures are read; WINTRUST_SIGNATURE_SETTINGS picks
 * one or counts them.  NovaOS has no catalog files, so the CryptCATAdmin
 * functions find no catalog for any hash (they do compute the hashes).
 */
#include <windows.h>

#define WTAPI __declspec(dllexport)

#define TRUST_E_PROVIDER_UNKNOWN_     ((LONG)0x800B0001L)
#define TRUST_E_ACTION_UNKNOWN_       ((LONG)0x800B0002L)
#define TRUST_E_SUBJECT_FORM_UNKNOWN_ ((LONG)0x800B0003L)
#define TRUST_E_NO_SIGNER_CERT_       ((LONG)0x80096002L)
#define TRUST_E_TIME_STAMP_           ((LONG)0x80096005L)
#define TRUST_E_BAD_DIGEST_           ((LONG)0x80096010L)
#define TRUST_E_NOSIGNATURE_          ((LONG)0x800B0100L)
#define CRYPT_E_NO_MATCH_             ((LONG)0x80092009L)
#define NTE_BAD_ALGID_                ((LONG)0x80090008L)

/* crypt32 */
typedef struct { DWORD cbData; BYTE *pbData; } BLOB_;
typedef struct { DWORD cbData; BYTE *pbData; DWORD cUnusedBits; } BIT_BLOB_;
typedef struct { LPSTR pszObjId; BLOB_ Parameters; } ALGID_;
typedef struct {
    DWORD dwVersion;
    BLOB_ SerialNumber;
    ALGID_ SignatureAlgorithm;
    BLOB_ Issuer;
    FILETIME NotBefore, NotAfter;
    BLOB_ Subject;
    struct { ALGID_ Algorithm; BIT_BLOB_ PublicKey; } SubjectPublicKeyInfo;
    BIT_BLOB_ IssuerUniqueId, SubjectUniqueId;
    DWORD cExtension;
    void *rgExtension;
} CERT_INFO_;
typedef struct { DWORD dwCertEncodingType; BYTE *pbCertEncoded; DWORD cbCertEncoded; CERT_INFO_ *pCertInfo; HANDLE hCertStore; } CERT_CONTEXT_;
typedef struct { LPSTR pszObjId; DWORD cValue; BLOB_ *rgValue; } ATTR_;
typedef struct { DWORD cAttr; ATTR_ *rgAttr; } ATTRS_;
typedef struct { DWORD dwErrorStatus, dwInfoStatus; } TRUST_STATUS_;
typedef struct {
    DWORD cbSize;
    const CERT_CONTEXT_ *pCertContext;
    TRUST_STATUS_ TrustStatus;
    void *pRevocationInfo, *pIssuanceUsage, *pApplicationUsage;
    LPCWSTR pwszExtendedErrorInfo;
} CHAIN_ELEMENT_;
typedef struct { DWORD cbSize; TRUST_STATUS_ TrustStatus; DWORD cElement; CHAIN_ELEMENT_ **rgpElement; } SIMPLE_CHAIN_;
typedef struct { DWORD cbSize; TRUST_STATUS_ TrustStatus; DWORD cChain; SIMPLE_CHAIN_ **rgpChain; } CHAIN_CONTEXT_;
typedef struct { DWORD cUsageIdentifier; LPSTR *rgpszUsageIdentifier; } ENHKEY_USAGE_;
typedef struct { DWORD cbSize; struct { DWORD dwType; ENHKEY_USAGE_ Usage; } RequestedUsage; } CHAIN_PARA_;
typedef struct { DWORD cbSize, dwFlags; void *pvExtraPolicyPara; } POLICY_PARA_;
typedef struct { DWORD cbSize, dwError; LONG lChainIndex, lElementIndex; void *pvExtraPolicyStatus; } POLICY_STATUS_;
typedef struct { DWORD cbSize; ULONG_PTR hCryptProv; DWORD dwSignerIndex, dwSignerType; void *pvSigner; } VERIFY_EX_PARA_;

#define ENC_ 0x10001                   /* X509_ASN_ENCODING | PKCS_7_ASN_ENCODING */
__declspec(dllimport) HANDLE WINAPI CryptMsgOpenToDecode(DWORD, DWORD, DWORD, HANDLE, void *, const void *);
__declspec(dllimport) BOOL WINAPI CryptMsgUpdate(HANDLE, const BYTE *, DWORD, BOOL);
__declspec(dllimport) BOOL WINAPI CryptMsgGetParam(HANDLE, DWORD, DWORD, void *, DWORD *);
__declspec(dllimport) BOOL WINAPI CryptMsgControl(HANDLE, DWORD, DWORD, const void *);
__declspec(dllimport) BOOL WINAPI CryptMsgClose(HANDLE);
__declspec(dllimport) BOOL WINAPI CryptMsgVerifyCountersignatureEncodedEx(ULONG_PTR, DWORD, const BYTE *, DWORD, const BYTE *, DWORD,
                                                                          DWORD, void *, DWORD, void *);
__declspec(dllimport) HANDLE WINAPI CertOpenStore(LPCSTR, DWORD, ULONG_PTR, DWORD, const void *);
__declspec(dllimport) BOOL WINAPI CertCloseStore(HANDLE, DWORD);
__declspec(dllimport) const CERT_CONTEXT_ *WINAPI CertGetSubjectCertificateFromStore(HANDLE, DWORD, CERT_INFO_ *);
__declspec(dllimport) const CERT_CONTEXT_ *WINAPI CertDuplicateCertificateContext(const CERT_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertFreeCertificateContext(const CERT_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertGetCertificateChain(HANDLE, const CERT_CONTEXT_ *, LPFILETIME, HANDLE, const CHAIN_PARA_ *,
                                                          DWORD, PVOID, const CHAIN_CONTEXT_ **);
__declspec(dllimport) VOID WINAPI CertFreeCertificateChain(const CHAIN_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertVerifyCertificateChainPolicy(LPCSTR, const CHAIN_CONTEXT_ *, POLICY_PARA_ *, POLICY_STATUS_ *);

/* bcrypt */
__declspec(dllimport) LONG WINAPI BCryptOpenAlgorithmProvider(void **, LPCWSTR, LPCWSTR, ULONG);
__declspec(dllimport) LONG WINAPI BCryptCloseAlgorithmProvider(void *, ULONG);
__declspec(dllimport) LONG WINAPI BCryptCreateHash(void *, void **, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
__declspec(dllimport) LONG WINAPI BCryptHashData(void *, PUCHAR, ULONG, ULONG);
__declspec(dllimport) LONG WINAPI BCryptFinishHash(void *, PUCHAR, ULONG, ULONG);
__declspec(dllimport) LONG WINAPI BCryptDestroyHash(void *);

/* wintrust.h */
typedef struct { DWORD cbStruct; LPCWSTR pcwszFilePath; HANDLE hFile; GUID *pgKnownSubject; } FILE_INFO_;
typedef struct {
    DWORD cbStruct;
    GUID gSubject;
    LPCWSTR pcwszDisplayName;
    DWORD cbMemObject;
    BYTE *pbMemObject;
    DWORD cbMemSignedMsg;
    BYTE *pbMemSignedMsg;
} BLOB_INFO_;
typedef struct { DWORD cbStruct, dwIndex, dwFlags, cSecondarySigs, dwVerifiedSigIndex; void *pCryptoPolicy; } SIG_SETTINGS_;
typedef struct {
    DWORD cbStruct;
    LPVOID pPolicyCallbackData, pSIPClientData;
    DWORD dwUIChoice, fdwRevocationChecks, dwUnionChoice;
    void *pInfo;                        /* the union: pFile, pCatalog, pBlob, pSgnr, pCert */
    DWORD dwStateAction;
    HANDLE hWVTStateData;
    WCHAR *pwszURLReference;
    DWORD dwProvFlags, dwUIContext;
    SIG_SETTINGS_ *pSignatureSettings;
} WINTRUST_DATA_;
#define WTD_CHOICE_FILE_          1
#define WTD_CHOICE_BLOB_          3
#define WTD_REVOKE_WHOLECHAIN_    1
#define WTD_STATEACTION_IGNORE_   0
#define WTD_STATEACTION_VERIFY_   1
#define WTD_STATEACTION_CLOSE_    2
#define WTD_REVOCATION_CHECK_NONE_ 0x10
#define WSS_VERIFY_SPECIFIC_      1
#define WSS_GET_SECONDARY_SIG_COUNT_ 2

typedef struct {
    DWORD cbStruct;
    const CERT_CONTEXT_ *pCert;
    BOOL fCommercial, fTrustedRoot, fSelfSigned, fTestCert;
    DWORD dwRevokedReason, dwConfidence, dwError;
    void *pTrustListContext;
    BOOL fTrustListSignerCert;
    void *pCtlContext;
    DWORD dwCtlError;
    BOOL fIsCyclic;
    CHAIN_ELEMENT_ *pChainElement;
} PROV_CERT_;
typedef struct PROV_SGNR_ {
    DWORD cbStruct;
    FILETIME sftVerifyAsOf;
    DWORD csCertChain;
    PROV_CERT_ *pasCertChain;
    DWORD dwSignerType;
    void *psSigner;                     /* CMSG_SIGNER_INFO */
    DWORD dwError;
    DWORD csCounterSigners;
    struct PROV_SGNR_ *pasCounterSigners;
    const CHAIN_CONTEXT_ *pChainContext;
} PROV_SGNR_;
typedef struct {
    DWORD cbStruct;
    WINTRUST_DATA_ *pWintrustData;
    BOOL fOpenedFile;
    HWND hWndParent;
    GUID *pgActionID;
    ULONG_PTR hProv;
    DWORD dwError, dwRegSecuritySettings, dwRegPolicySettings;
    void *psPfns;
    DWORD cdwTrustStepErrors;
    DWORD *padwTrustStepErrors;
    DWORD chStores;
    HANDLE *pahStores;
    DWORD dwEncoding;
    HANDLE hMsg;
    DWORD csSigners;
    PROV_SGNR_ *pasSigners;
    DWORD csProvPrivData;
    void *pasProvPrivData;
    DWORD dwSubjectChoice;
    void *pPDSip;
    char *pszUsageOID;
    BOOL fRecallWithState;
    FILETIME sftSystemTime;
    char *pszCTLSignerUsageOID;
    DWORD dwProvFlags, dwFinalError;
    void *pRequestUsage;
    DWORD dwTrustPubSettings, dwUIStateFlags;
    void *pSigState, *pSigSettings;
} PROV_DATA_;

static void *zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void zfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static int bytes_cmp(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = a, *y = b;
    for (SIZE_T i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* -----------------------------------------------------------------------
 * DER
 * ----------------------------------------------------------------------- */
typedef struct { const BYTE *p; SIZE_T n; } Span;

static BOOL der_next(const BYTE **p, const BYTE *end, BYTE *tag, Span *val, Span *all)
{
    const BYTE *s = *p, *q = s;
    if (end - q < 2) return FALSE;
    BYTE t = *q++;
    SIZE_T len = *q++;
    if (len & 0x80) {
        int k = (int)(len & 0x7F);
        if (k == 0 || k > 4 || end - q < k) return FALSE;
        len = 0;
        while (k--) len = len << 8 | *q++;
    }
    if ((SIZE_T)(end - q) < len) return FALSE;
    if (tag) *tag = t;
    if (val) { val->p = q; val->n = len; }
    if (all) { all->p = s; all->n = (SIZE_T)(q - s) + len; }
    *p = q + len;
    return TRUE;
}

static BOOL der_expect(const BYTE **p, const BYTE *end, BYTE want, Span *val, Span *all)
{
    BYTE t;
    const BYTE *save = *p;
    if (!der_next(p, end, &t, val, all) || t != want) { *p = save; return FALSE; }
    return TRUE;
}

/* An OID's encoding as dotted text */
static void oid_text(Span oid, char *out, SIZE_T cap)
{
    SIZE_T o = 0;
    unsigned long v = 0;
    int first = 1;
    out[0] = 0;
    for (SIZE_T i = 0; i < oid.n; i++) {
        v = v << 7 | (oid.p[i] & 0x7F);
        if (oid.p[i] & 0x80) continue;
        char tmp[24];
        int k = 0;
        unsigned long parts[2] = { v, 0 };
        int np = 1;
        if (first) { parts[0] = v < 80 ? v / 40 : 2; parts[1] = v < 80 ? v % 40 : v - 80; np = 2; first = 0; }
        for (int j = 0; j < np; j++) {
            unsigned long x = parts[j];
            k = 0;
            do { tmp[k++] = (char)('0' + x % 10); x /= 10; } while (x);
            if (o && o + 1 < cap) out[o++] = '.';
            while (k && o + 1 < cap) out[o++] = tmp[--k];
        }
        v = 0;
    }
    out[o < cap ? o : cap - 1] = 0;
}

static BOOL oid_is(Span oid, const char *text)
{
    char s[96];
    oid_text(oid, s, sizeof(s));
    return str_eq(s, text);
}

/* UTCTime or GeneralizedTime to a FILETIME */
static BOOL der_time(BYTE tag, Span v, FILETIME *ft)
{
    SYSTEMTIME st = { 0 };
    int d[14], n = 0;
    for (SIZE_T i = 0; i < v.n && n < 14 && v.p[i] >= '0' && v.p[i] <= '9'; i++) d[n++] = v.p[i] - '0';
    int o = 0;
    if (tag == 0x17 && n >= 12) { int y = d[0] * 10 + d[1]; st.wYear = (WORD)(y < 50 ? 2000 + y : 1900 + y); o = 2; }
    else if (tag == 0x18 && n >= 14) { st.wYear = (WORD)(d[0] * 1000 + d[1] * 100 + d[2] * 10 + d[3]); o = 4; }
    else return FALSE;
    st.wMonth = (WORD)(d[o] * 10 + d[o + 1]);
    st.wDay = (WORD)(d[o + 2] * 10 + d[o + 3]);
    st.wHour = (WORD)(d[o + 4] * 10 + d[o + 5]);
    st.wMinute = (WORD)(d[o + 6] * 10 + d[o + 7]);
    st.wSecond = (WORD)(d[o + 8] * 10 + d[o + 9]);
    return SystemTimeToFileTime(&st, ft);
}

/* -----------------------------------------------------------------------
 * Hashes (bcrypt)
 * ----------------------------------------------------------------------- */
typedef struct { void *alg, *h; DWORD len; } Hash;

/* The hash an AlgorithmIdentifier's OID names */
static BOOL hash_start(Span oid, Hash *h)
{
    static const struct { const char *oid; const WCHAR *name; DWORD len; } t[] = {
        { "1.3.14.3.2.26", L"SHA1", 20 }, { "2.16.840.1.101.3.4.2.1", L"SHA256", 32 },
        { "2.16.840.1.101.3.4.2.2", L"SHA384", 48 }, { "2.16.840.1.101.3.4.2.3", L"SHA512", 64 },
        { "1.2.840.113549.2.5", L"MD5", 16 },
    };
    for (int i = 0; i < 5; i++)
        if (oid_is(oid, t[i].oid)) {
            h->len = t[i].len;
            if (BCryptOpenAlgorithmProvider(&h->alg, t[i].name, NULL, 0)) return FALSE;
            if (BCryptCreateHash(h->alg, &h->h, NULL, 0, NULL, 0, 0)) { BCryptCloseAlgorithmProvider(h->alg, 0); return FALSE; }
            return TRUE;
        }
    return FALSE;
}
static void hash_data(Hash *h, const void *p, SIZE_T n)
{
    while (n) {
        ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        BCryptHashData(h->h, (PUCHAR)p, k, 0);
        p = (const BYTE *)p + k;
        n -= k;
    }
}
static void hash_end(Hash *h, BYTE *out)
{
    BCryptFinishHash(h->h, out, h->len, 0);
    BCryptDestroyHash(h->h);
    BCryptCloseAlgorithmProvider(h->alg, 0);
}

/* -----------------------------------------------------------------------
 * The subject: a file (by name or handle) or a PE image in memory
 * ----------------------------------------------------------------------- */
typedef struct { HANDLE f; const BYTE *mem; ULONGLONG size; BYTE *buf; DWORD cap; } Subject;

static const BYTE *subj_at(Subject *s, ULONGLONG off, DWORD n)
{
    if (off + n > s->size) return NULL;
    if (s->mem) return s->mem + off;
    if (n > s->cap) {
        zfree(s->buf);
        s->buf = HeapAlloc(GetProcessHeap(), 0, n);
        s->cap = s->buf ? n : 0;
        if (!s->buf) return NULL;
    }
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    if (!SetFilePointerEx(s->f, li, NULL, FILE_BEGIN) || !ReadFile(s->f, s->buf, n, &got, NULL) || got != n) return NULL;
    return s->buf;
}

static void subj_hash(Subject *s, Hash *h, ULONGLONG from, ULONGLONG to)
{
    while (from < to) {
        DWORD k = to - from > (1u << 20) ? 1u << 20 : (DWORD)(to - from);
        const BYTE *p = subj_at(s, from, k);
        if (!p) return;
        hash_data(h, p, k);
        from += k;
    }
}

/* The PE layout Authenticode needs: where the checksum and the
 * certificate table's directory entry are, and the table */
typedef struct { DWORD checksum, dirent, cert_off, cert_size; } PeLayout;

static LONG pe_layout(Subject *s, PeLayout *pl)
{
    const BYTE *mz = subj_at(s, 0, 64);
    if (!mz || mz[0] != 'M' || mz[1] != 'Z') return TRUST_E_SUBJECT_FORM_UNKNOWN_;
    DWORD pe = *(const DWORD *)(mz + 60);
    const BYTE *nt = subj_at(s, pe, 24 + 2);
    if (!nt || bytes_cmp(nt, "PE\0\0", 4)) return TRUST_E_SUBJECT_FORM_UNKNOWN_;
    WORD magic = *(const WORD *)(nt + 24);
    if (magic != 0x10B && magic != 0x20B) return TRUST_E_SUBJECT_FORM_UNKNOWN_;
    pl->checksum = pe + 24 + 64;
    pl->dirent = pe + 24 + (magic == 0x20B ? 112 : 96) + 4 * 8;
    const BYTE *d = subj_at(s, pl->dirent, 8);
    if (!d) return TRUST_E_SUBJECT_FORM_UNKNOWN_;
    pl->cert_off = *(const DWORD *)d;
    pl->cert_size = *(const DWORD *)(d + 4);
    if (!pl->cert_off || !pl->cert_size) return TRUST_E_NOSIGNATURE_;
    if ((ULONGLONG)pl->cert_off + pl->cert_size > s->size || pl->cert_off < pl->dirent + 8) return TRUST_E_NOSIGNATURE_;
    return 0;
}

/* The Authenticode digest: everything before the certificate table but
 * the checksum and the table's directory entry */
static BOOL pe_digest(Subject *s, const PeLayout *pl, Span alg_oid, BYTE *out, DWORD *len)
{
    Hash h;
    if (!hash_start(alg_oid, &h)) return FALSE;
    subj_hash(s, &h, 0, pl->checksum);
    subj_hash(s, &h, pl->checksum + 4, pl->dirent);
    subj_hash(s, &h, pl->dirent + 8, pl->cert_off);
    *len = h.len;
    hash_end(&h, out);
    return TRUE;
}

/* The PKCS #7 signature of the certificate table's first signed-data entry */
static BYTE *pe_signature(Subject *s, const PeLayout *pl, DWORD *len)
{
    for (ULONGLONG pos = pl->cert_off; pos + 8 <= (ULONGLONG)pl->cert_off + pl->cert_size;) {
        const BYTE *wc = subj_at(s, pos, 8);
        if (!wc) return NULL;
        DWORD n = *(const DWORD *)wc;
        WORD kind = *(const WORD *)(wc + 6);
        if (n <= 8 || n > pl->cert_size) return NULL;
        if (kind == 2) {                                /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */
            const BYTE *b = subj_at(s, pos + 8, n - 8);
            BYTE *copy = b ? HeapAlloc(GetProcessHeap(), 0, n - 8) : NULL;
            if (copy) { CopyMemory(copy, b, n - 8); *len = n - 8; }
            return copy;
        }
        pos += (n + 7) & ~7u;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Signatures
 * ----------------------------------------------------------------------- */
typedef struct {
    HANDLE msg, store;
    const CERT_CONTEXT_ *signer;
    const CHAIN_CONTEXT_ *chain;
    BOOL timestamped;
    FILETIME when;                      /* the timestamp's time */
    const CERT_CONTEXT_ *tsa;
    const CHAIN_CONTEXT_ *tsa_chain;
    HANDLE tsa_msg, tsa_store;
    DWORD nested;                       /* nested signatures (on the primary) */
} Sig;

static void sig_free(Sig *g)
{
    if (g->chain) CertFreeCertificateChain(g->chain);
    if (g->tsa_chain) CertFreeCertificateChain(g->tsa_chain);
    if (g->signer) CertFreeCertificateContext(g->signer);
    if (g->tsa) CertFreeCertificateContext(g->tsa);
    if (g->store) CertCloseStore(g->store, 0);
    if (g->tsa_store) CertCloseStore(g->tsa_store, 0);
    if (g->msg) CryptMsgClose(g->msg);
    if (g->tsa_msg) CryptMsgClose(g->tsa_msg);
    ZeroMemory(g, sizeof(*g));
}

static HANDLE open_msg(const BYTE *b, DWORD n)
{
    HANDLE m = CryptMsgOpenToDecode(ENC_, 0, 0, 0, NULL, NULL);
    if (m && !CryptMsgUpdate(m, b, n, TRUE)) { CryptMsgClose(m); m = 0; }
    return m;
}

/* A message parameter, allocated */
static void *msg_param(HANDLE m, DWORD type, DWORD index, DWORD *len)
{
    DWORD n = 0;
    if (!CryptMsgGetParam(m, type, index, NULL, &n)) return NULL;
    void *p = zalloc(n + 1);
    if (p && !CryptMsgGetParam(m, type, index, p, &n)) { zfree(p); return NULL; }
    if (len) *len = n;
    return p;
}

/* The certificate a message's signer @i signed with, verified to have
 * signed it */
static const CERT_CONTEXT_ *msg_signer(HANDLE m, HANDLE store, LONG *err)
{
    CERT_INFO_ *id = msg_param(m, 7 /* CMSG_SIGNER_CERT_INFO_PARAM */, 0, NULL);
    const CERT_CONTEXT_ *c = id ? CertGetSubjectCertificateFromStore(store, ENC_, id) : NULL;
    zfree(id);
    if (!c) { *err = TRUST_E_NO_SIGNER_CERT_; return NULL; }
    VERIFY_EX_PARA_ v = { sizeof(v), 0, 0, 2 /* CMSG_VERIFY_SIGNER_CERT */, (void *)c };
    if (!CryptMsgControl(m, 0, 19 /* CMSG_CTRL_VERIFY_SIGNATURE_EX */, &v)) {
        *err = TRUST_E_BAD_DIGEST_;
        CertFreeCertificateContext(c);
        return NULL;
    }
    return c;
}

/* A chain for @c at @when (NULL: now) for the usage @usage, and the
 * Authenticode policy's verdict on it */
static LONG check_chain(const CERT_CONTEXT_ *c, const FILETIME *when, HANDLE extra, const char *usage, LPCSTR policy,
                        DWORD chain_flags, const CHAIN_CONTEXT_ **out)
{
    LPSTR u[1] = { (LPSTR)usage };
    CHAIN_PARA_ para = { sizeof(para), { 0 /* AND */, { 1, u } } };
    FILETIME t = when ? *when : (FILETIME){ 0 };
    if (!CertGetCertificateChain(0, c, when ? &t : NULL, extra, &para, chain_flags, NULL, out)) return (LONG)GetLastError();
    POLICY_PARA_ pp = { sizeof(pp), 0, NULL };
    POLICY_STATUS_ ps = { sizeof(ps), 0, -1, -1, NULL };
    if (!CertVerifyCertificateChainPolicy(policy, *out, &pp, &ps)) return (LONG)GetLastError();
    return (LONG)ps.dwError;
}

/* The first value of attribute @oid among a signer's unauthenticated ones */
static BOOL unauth_attr(HANDLE m, const char *oid, DWORD index, Span *v, ATTRS_ **keep)
{
    ATTRS_ *a = msg_param(m, 10 /* CMSG_SIGNER_UNAUTH_ATTR_PARAM */, 0, NULL);
    if (!a) return FALSE;
    for (DWORD i = 0; i < a->cAttr; i++)
        if (str_eq(a->rgAttr[i].pszObjId, oid) && index < a->rgAttr[i].cValue) {
            v->p = a->rgAttr[i].rgValue[index].pbData;
            v->n = a->rgAttr[i].rgValue[index].cbData;
            *keep = a;
            return TRUE;
        }
    zfree(a);
    return FALSE;
}

static DWORD unauth_count(HANDLE m, const char *oid)
{
    ATTRS_ *a = msg_param(m, 10, 0, NULL);
    DWORD n = 0;
    for (DWORD i = 0; a && i < a->cAttr; i++) if (str_eq(a->rgAttr[i].pszObjId, oid)) n += a->rgAttr[i].cValue;
    zfree(a);
    return n;
}

/* A SignerInfo's issuer and serial number, as a CERT_INFO naming its cert */
static BOOL signer_id(Span si, CERT_INFO_ *id, BYTE *serial, DWORD cap)
{
    const BYTE *p = si.p, *end = si.p + si.n;
    Span seq, v, iss, ser;
    if (!der_expect(&p, end, 0x30, &seq, NULL)) return FALSE;
    p = seq.p;
    end = seq.p + seq.n;
    if (!der_expect(&p, end, 0x02, &v, NULL) || !der_expect(&p, end, 0x30, &v, NULL)) return FALSE;
    const BYTE *q = v.p;
    if (!der_expect(&q, v.p + v.n, 0x30, NULL, &iss) || !der_expect(&q, v.p + v.n, 0x02, &ser, NULL) || ser.n > cap) return FALSE;
    ZeroMemory(id, sizeof(*id));
    id->Issuer.cbData = (DWORD)iss.n;
    id->Issuer.pbData = (BYTE *)iss.p;
    for (SIZE_T i = 0; i < ser.n; i++) serial[i] = ser.p[ser.n - 1 - i];
    id->SerialNumber.cbData = (DWORD)ser.n;
    id->SerialNumber.pbData = serial;
    return TRUE;
}

/* An authenticated attribute's first value, from a SignerInfo's encoding */
static BOOL signed_attr(Span si, const char *oid, BYTE *tag, Span *val)
{
    const BYTE *p = si.p, *end = si.p + si.n;
    Span seq, attrs;
    if (!der_expect(&p, end, 0x30, &seq, NULL)) return FALSE;
    p = seq.p;
    end = seq.p + seq.n;
    der_next(&p, end, NULL, NULL, NULL);                /* version */
    der_next(&p, end, NULL, NULL, NULL);                /* sid */
    der_next(&p, end, NULL, NULL, NULL);                /* digestAlgorithm */
    if (!der_expect(&p, end, 0xA0, &attrs, NULL)) return FALSE;
    for (const BYTE *q = attrs.p; q < attrs.p + attrs.n;) {
        Span a, o, vals;
        if (!der_next(&q, attrs.p + attrs.n, NULL, &a, NULL)) return FALSE;
        const BYTE *r = a.p;
        if (!der_expect(&r, a.p + a.n, 0x06, &o, NULL) || !der_expect(&r, a.p + a.n, 0x31, &vals, NULL)) continue;
        if (!oid_is(o, oid)) continue;
        const BYTE *x = vals.p;
        return der_next(&x, vals.p + vals.n, tag, val, NULL);
    }
    return FALSE;
}

/* The signature's timestamp: a PKCS #9 countersignature by a time-stamping
 * authority over the signer's encrypted digest, or an RFC 3161 token whose
 * message imprint is that digest's hash.  0 (and g->when set) when it
 * verifies, 1 when there is none, else the error. */
static LONG check_timestamp(Sig *g, DWORD chain_flags)
{
    ATTRS_ *keep = NULL;
    Span v;
    LONG err = TRUST_E_TIME_STAMP_;
    DWORD sl = 0;
    BYTE *outer = msg_param(g->msg, 28 /* CMSG_ENCODED_SIGNER */, 0, &sl);
    if (!outer) return 1;
    if (unauth_attr(g->msg, "1.2.840.113549.1.9.6", 0, &v, &keep)) {               /* countersignature */
        CERT_INFO_ id;
        BYTE serial[64];
        BYTE tag;
        Span t;
        if (signer_id(v, &id, serial, sizeof(serial)) &&
            (g->tsa = CertGetSubjectCertificateFromStore(g->store, ENC_, &id)) != NULL &&
            CryptMsgVerifyCountersignatureEncodedEx(0, ENC_, outer, sl, v.p, (DWORD)v.n, 2, (void *)g->tsa, 0, NULL) &&
            signed_attr(v, "1.2.840.113549.1.9.5", &tag, &t) && der_time(tag, t, &g->when)) {    /* signingTime */
            err = check_chain(g->tsa, &g->when, g->store, "1.3.6.1.5.5.7.3.8", (LPCSTR)3, chain_flags, &g->tsa_chain);
        }
    } else if (unauth_attr(g->msg, "1.3.6.1.4.1.311.3.3.1", 0, &v, &keep)) {       /* RFC 3161 token */
        DWORD clen = 0;
        BYTE *tst;
        if ((g->tsa_msg = open_msg(v.p, (DWORD)v.n)) != 0 &&
            (g->tsa_store = CertOpenStore((LPCSTR)1, ENC_, 0, 0, g->tsa_msg)) != 0 &&
            (g->tsa = msg_signer(g->tsa_msg, g->tsa_store, &err)) != NULL &&
            (tst = msg_param(g->tsa_msg, 2 /* CMSG_CONTENT_PARAM */, 0, &clen)) != NULL) {
            /* TSTInfo { version, policy, messageImprint { alg, digest }, serial, genTime, ... } */
            const BYTE *p = tst, *end = tst + clen;
            Span seq, mi, alg, aoid, dig, gt;
            BYTE tag;
            err = TRUST_E_TIME_STAMP_;
            if (der_expect(&p, end, 0x30, &seq, NULL)) {
                p = seq.p;
                end = seq.p + seq.n;
                der_next(&p, end, NULL, NULL, NULL);
                der_next(&p, end, NULL, NULL, NULL);
                if (der_expect(&p, end, 0x30, &mi, NULL) && der_next(&p, end, NULL, NULL, NULL) &&
                    der_next(&p, end, &tag, &gt, NULL) && der_time(tag, gt, &g->when)) {
                    const BYTE *q = mi.p;
                    Hash h;
                    BYTE want[64];
                    /* the imprint is the hash of the signer's encrypted digest */
                    Span sigval = { 0 };
                    const BYTE *s = outer, *se = outer + sl;
                    Span sseq, x;
                    BYTE t;
                    if (der_expect(&s, se, 0x30, &sseq, NULL)) {
                        s = sseq.p;
                        se = sseq.p + sseq.n;
                        while (der_next(&s, se, &t, &x, NULL)) if (t == 0x04) { sigval = x; break; }
                    }
                    if (der_expect(&q, mi.p + mi.n, 0x30, &alg, NULL) && der_expect(&q, mi.p + mi.n, 0x04, &dig, NULL)) {
                        const BYTE *a = alg.p;
                        if (der_expect(&a, alg.p + alg.n, 0x06, &aoid, NULL) && hash_start(aoid, &h)) {
                            hash_data(&h, sigval.p, sigval.n);
                            hash_end(&h, want);
                            if (sigval.n && dig.n == h.len && !bytes_cmp(dig.p, want, h.len))
                                err = check_chain(g->tsa, &g->when, g->tsa_store, "1.3.6.1.5.5.7.3.8", (LPCSTR)3,
                                                  chain_flags, &g->tsa_chain);
                        }
                    }
                }
            }
            zfree(tst);
        }
    } else err = 1;
    zfree(keep);
    zfree(outer);
    return err;
}

/* Signature @blob (a PKCS #7 SignedData) over a PE whose Authenticode
 * digest the subject gives */
static LONG verify_signature(const BYTE *blob, DWORD len, Subject *s, const PeLayout *pl, DWORD chain_flags, Sig *g)
{
    ZeroMemory(g, sizeof(*g));
    if (!(g->msg = open_msg(blob, len))) return TRUST_E_NOSIGNATURE_;
    char *inner = msg_param(g->msg, 4 /* CMSG_INNER_CONTENT_TYPE_PARAM */, 0, NULL);
    BOOL spc = inner && str_eq(inner, "1.3.6.1.4.1.311.2.1.4");                    /* SPC_INDIRECT_DATA_OBJID */
    zfree(inner);
    if (!spc) return TRUST_E_NOSIGNATURE_;
    /* SpcIndirectDataContent { SpcAttributeTypeAndOptionalValue, DigestInfo { alg, digest } } */
    DWORD clen = 0;
    BYTE *content = msg_param(g->msg, 2, 0, &clen);
    if (!content) return TRUST_E_NOSIGNATURE_;
    LONG err = TRUST_E_NOSIGNATURE_;
    const BYTE *p = content, *end = content + clen;
    Span seq, di, alg, oid, dig;
    if (der_expect(&p, end, 0x30, &seq, NULL)) {
        p = seq.p;
        end = seq.p + seq.n;
        if (der_expect(&p, end, 0x30, NULL, NULL) && der_expect(&p, end, 0x30, &di, NULL)) {
            const BYTE *q = di.p;
            if (der_expect(&q, di.p + di.n, 0x30, &alg, NULL) && der_expect(&q, di.p + di.n, 0x04, &dig, NULL)) {
                const BYTE *a = alg.p;
                BYTE got[64];
                DWORD glen = 0;
                if (!der_expect(&a, alg.p + alg.n, 0x06, &oid, NULL) || !pe_digest(s, pl, oid, got, &glen)) err = NTE_BAD_ALGID_;
                else if (glen != dig.n || bytes_cmp(got, dig.p, glen)) err = TRUST_E_BAD_DIGEST_;
                else err = 0;
            }
        }
    }
    zfree(content);
    if (err) return err;
    if (!(g->store = CertOpenStore((LPCSTR)1 /* CERT_STORE_PROV_MSG */, ENC_, 0, 0, g->msg))) return TRUST_E_NOSIGNATURE_;
    if (!(g->signer = msg_signer(g->msg, g->store, &err))) return err;
    g->nested = unauth_count(g->msg, "1.3.6.1.4.1.311.2.4.1");
    LONG ts = check_timestamp(g, chain_flags);
    if (ts < 0) return ts;
    g->timestamped = ts == 0;
    return check_chain(g->signer, g->timestamped ? &g->when : NULL, g->store, "1.3.6.1.5.5.7.3.3", (LPCSTR)2 /* AUTHENTICODE */,
                       chain_flags, &g->chain);
}

/* -----------------------------------------------------------------------
 * The provider's state (WTD_STATEACTION_VERIFY) and its helpers
 * ----------------------------------------------------------------------- */
typedef struct {
    PROV_DATA_ data;
    PROV_SGNR_ sgnr, counter;
    HANDLE stores[1];
    Sig sig;
    DWORD steps[38];
} State;

static void fill_certs(PROV_SGNR_ *sg, const CHAIN_CONTEXT_ *ch)
{
    if (!ch || !ch->cChain) return;
    const SIMPLE_CHAIN_ *sc = ch->rgpChain[0];
    sg->pasCertChain = zalloc(sc->cElement * sizeof(PROV_CERT_) + 1);
    if (!sg->pasCertChain) return;
    sg->csCertChain = sc->cElement;
    sg->pChainContext = ch;
    for (DWORD i = 0; i < sc->cElement; i++) {
        PROV_CERT_ *pc = &sg->pasCertChain[i];
        CHAIN_ELEMENT_ *e = sc->rgpElement[i];
        pc->cbStruct = sizeof(*pc);
        pc->pCert = e->pCertContext ? CertDuplicateCertificateContext(e->pCertContext) : NULL;
        pc->fSelfSigned = (e->TrustStatus.dwInfoStatus & 0x8) != 0;
        pc->fTrustedRoot = i + 1 == sc->cElement && pc->fSelfSigned && !(e->TrustStatus.dwErrorStatus & 0x20);
        pc->dwError = e->TrustStatus.dwErrorStatus;
        pc->pChainElement = e;
    }
}

static void free_certs(PROV_SGNR_ *sg)
{
    for (DWORD i = 0; i < sg->csCertChain; i++) if (sg->pasCertChain[i].pCert) CertFreeCertificateContext(sg->pasCertChain[i].pCert);
    zfree(sg->pasCertChain);
    zfree(sg->psSigner);
}

static State *state_new(WINTRUST_DATA_ *wd, GUID *action, Sig *g, LONG err)
{
    State *st = zalloc(sizeof(*st));
    if (!st) return NULL;
    st->sig = *g;
    ZeroMemory(g, sizeof(*g));
    PROV_DATA_ *d = &st->data;
    d->cbStruct = sizeof(*d);
    d->pWintrustData = wd;
    d->pgActionID = action;
    d->dwEncoding = ENC_;
    d->hMsg = st->sig.msg;
    d->cdwTrustStepErrors = 38;
    d->padwTrustStepErrors = st->steps;
    d->dwSubjectChoice = wd->dwUnionChoice;
    d->dwFinalError = (DWORD)err;
    d->dwProvFlags = wd->dwProvFlags;
    GetSystemTimeAsFileTime(&d->sftSystemTime);
    if (st->sig.store) { st->stores[0] = st->sig.store; d->chStores = 1; d->pahStores = st->stores; }
    if (st->sig.signer) {
        PROV_SGNR_ *sg = &st->sgnr;
        sg->cbStruct = sizeof(*sg);
        sg->sftVerifyAsOf = st->sig.timestamped ? st->sig.when : d->sftSystemTime;
        sg->psSigner = msg_param(st->sig.msg, 6 /* CMSG_SIGNER_INFO_PARAM */, 0, NULL);
        sg->dwError = (DWORD)err;
        fill_certs(sg, st->sig.chain);
        if (st->sig.timestamped) {
            PROV_SGNR_ *cs = &st->counter;
            cs->cbStruct = sizeof(*cs);
            cs->sftVerifyAsOf = st->sig.when;
            cs->dwSignerType = 1;                       /* SGNR_TYPE_TIMESTAMP */
            if (st->sig.tsa_msg) cs->psSigner = msg_param(st->sig.tsa_msg, 6, 0, NULL);
            fill_certs(cs, st->sig.tsa_chain);
            sg->csCounterSigners = 1;
            sg->pasCounterSigners = cs;
        }
        d->csSigners = 1;
        d->pasSigners = sg;
    }
    return st;
}

static void state_free(State *st)
{
    if (!st) return;
    st->sgnr.pChainContext = st->counter.pChainContext = NULL;       /* (Sig owns the chains) */
    free_certs(&st->sgnr);
    free_certs(&st->counter);
    sig_free(&st->sig);
    zfree(st);
}

WTAPI PVOID WINAPI WTHelperProvDataFromStateData(HANDLE h) { return h; }

WTAPI PVOID WINAPI WTHelperGetProvSignerFromChain(PVOID prov, DWORD signer, BOOL counter, DWORD counter_index)
{
    PROV_DATA_ *d = prov;
    if (!d || signer >= d->csSigners) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    PROV_SGNR_ *sg = &d->pasSigners[signer];
    if (!counter) return sg;
    if (counter_index >= sg->csCounterSigners) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return &sg->pasCounterSigners[counter_index];
}

WTAPI PVOID WINAPI WTHelperGetProvCertFromChain(PVOID sgnr, DWORD idx)
{
    PROV_SGNR_ *sg = sgnr;
    if (!sg || idx >= sg->csCertChain) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return &sg->pasCertChain[idx];
}

/* -----------------------------------------------------------------------
 * WinVerifyTrust
 * ----------------------------------------------------------------------- */
static const GUID GENERIC_VERIFY_V2 = { 0x00AAC56B, 0xCD44, 0x11D0, { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } };
static const GUID DRIVER_VERIFY = { 0xF750E6C3, 0x38EE, 0x11D1, { 0x85, 0xE5, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } };
static const GUID PUBLISHED_SOFTWARE = { 0x64B9D180, 0x8DA2, 0x11CF, { 0x87, 0x36, 0x00, 0xAA, 0x00, 0xA4, 0x85, 0x53 } };

static LONG verify(GUID *action, WINTRUST_DATA_ *wd)
{
    if (!action || (bytes_cmp(action, &GENERIC_VERIFY_V2, sizeof(GUID)) && bytes_cmp(action, &DRIVER_VERIFY, sizeof(GUID)) &&
                    bytes_cmp(action, &PUBLISHED_SOFTWARE, sizeof(GUID))))
        return TRUST_E_PROVIDER_UNKNOWN_;
    if (!wd || wd->cbStruct < 0x28) return ERROR_INVALID_PARAMETER;
    if (wd->dwStateAction == WTD_STATEACTION_CLOSE_) {
        state_free(wd->hWVTStateData);
        wd->hWVTStateData = NULL;
        return 0;
    }
    SIG_SETTINGS_ *ss = wd->cbStruct >= sizeof(WINTRUST_DATA_) ? wd->pSignatureSettings : NULL;

    Subject s = { INVALID_HANDLE_VALUE, NULL, 0, NULL, 0 };
    HANDLE opened = INVALID_HANDLE_VALUE;
    LONG err = 0;
    if (wd->dwUnionChoice == WTD_CHOICE_FILE_ && wd->pInfo) {
        const FILE_INFO_ *fi = wd->pInfo;
        s.f = fi->hFile && fi->hFile != INVALID_HANDLE_VALUE ? fi->hFile : INVALID_HANDLE_VALUE;
        if (s.f == INVALID_HANDLE_VALUE && fi->pcwszFilePath)
            s.f = opened = CreateFileW(fi->pcwszFilePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER sz;
        if (s.f == INVALID_HANDLE_VALUE) err = (LONG)GetLastError() ? (LONG)(0x80070000 | (GetLastError() & 0xFFFF)) : TRUST_E_SUBJECT_FORM_UNKNOWN_;
        else if (GetFileSizeEx(s.f, &sz)) s.size = (ULONGLONG)sz.QuadPart;
    } else if (wd->dwUnionChoice == WTD_CHOICE_BLOB_ && wd->pInfo) {
        const BLOB_INFO_ *bi = wd->pInfo;
        s.mem = bi->pbMemObject;
        s.size = bi->cbMemObject;
    } else err = TRUST_E_NOSIGNATURE_;               /* catalogs (none exist), signers, certificates */

    PeLayout pl;
    Sig g;
    ZeroMemory(&g, sizeof(g));
    if (!err) err = pe_layout(&s, &pl);
    DWORD chain_flags = wd->fdwRevocationChecks == WTD_REVOKE_WHOLECHAIN_ && !(wd->dwProvFlags & WTD_REVOCATION_CHECK_NONE_) ?
                        0x40000000 /* CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT */ : 0;
    if (!err) {
        DWORD len = 0;
        BYTE *blob = pe_signature(&s, &pl, &len);
        if (!blob) err = TRUST_E_NOSIGNATURE_;
        else {
            err = verify_signature(blob, len, &s, &pl, chain_flags, &g);
            DWORD want = ss && (ss->dwFlags & WSS_VERIFY_SPECIFIC_) ? ss->dwIndex : 0;
            if (ss) { ss->cSecondarySigs = g.msg ? g.nested : 0; ss->dwVerifiedSigIndex = 0; }
            if (ss && (ss->dwFlags & WSS_GET_SECONDARY_SIG_COUNT_) && !(ss->dwFlags & WSS_VERIFY_SPECIFIC_)) {
                /* (the count only: the primary's verdict stands) */
            } else if (want && g.msg) {
                /* a nested signature, each a whole SignedData in the primary's attribute */
                ATTRS_ *keep = NULL;
                Span v;
                if (!unauth_attr(g.msg, "1.3.6.1.4.1.311.2.4.1", want - 1, &v, &keep)) {
                    sig_free(&g);
                    err = TRUST_E_NOSIGNATURE_;
                } else {
                    BYTE *copy = HeapAlloc(GetProcessHeap(), 0, v.n);
                    DWORD n = (DWORD)v.n;
                    if (copy) CopyMemory(copy, v.p, n);
                    zfree(keep);
                    sig_free(&g);
                    err = copy ? verify_signature(copy, n, &s, &pl, chain_flags, &g) : (LONG)ERROR_NOT_ENOUGH_MEMORY;
                    zfree(copy);
                    if (ss) ss->dwVerifiedSigIndex = want;
                }
            }
            zfree(blob);
        }
    }
    if (opened != INVALID_HANDLE_VALUE) CloseHandle(opened);
    zfree(s.buf);
    if (wd->dwStateAction == WTD_STATEACTION_VERIFY_) wd->hWVTStateData = state_new(wd, action, &g, err);
    sig_free(&g);
    return err;
}

WTAPI LONG WINAPI WinVerifyTrust(HWND w, GUID *action, LPVOID data)
{
    (void)w;
    LONG r = verify(action, data);
    SetLastError((DWORD)r);
    return r;
}

WTAPI HRESULT WINAPI WinVerifyTrustEx(HWND w, GUID *action, LPVOID data) { return WinVerifyTrust(w, action, data); }

/* -----------------------------------------------------------------------
 * Catalogs: there are none; the hashes they are looked up by are made
 * ----------------------------------------------------------------------- */
typedef struct { DWORD magic; BOOL sha256; } CatAdmin;
#define CAT_MAGIC 0x43415441

WTAPI BOOL WINAPI CryptCATAdminAcquireContext2(HANDLE *h, const GUID *sub, LPCWSTR alg, const void *pol, DWORD flags)
{
    (void)sub; (void)pol; (void)flags;
    if (!h) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    CatAdmin *c = zalloc(sizeof(*c));
    if (!c) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    c->magic = CAT_MAGIC;
    c->sha256 = alg && (alg[3] == '2' || alg[3] == 0 ? alg[3] == '2' : 0);    /* L"SHA256" (L"SHA1": the default) */
    *h = c;
    return TRUE;
}
WTAPI BOOL WINAPI CryptCATAdminAcquireContext(HANDLE *h, const GUID *sub, DWORD flags) { return CryptCATAdminAcquireContext2(h, sub, NULL, NULL, flags); }
WTAPI BOOL WINAPI CryptCATAdminReleaseContext(HANDLE h, DWORD flags)
{
    (void)flags;
    CatAdmin *c = h;
    if (!c || c->magic != CAT_MAGIC) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    c->magic = 0;
    zfree(c);
    return TRUE;
}

/* A file's catalog hash: a PE's Authenticode digest, else the file's */
static BOOL calc_hash(HANDLE f, BOOL sha256, DWORD *n, BYTE *hash)
{
    DWORD len = sha256 ? 32 : 20;
    if (!f || f == INVALID_HANDLE_VALUE || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!hash || *n < len) { *n = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    Subject s = { f, NULL, 0, NULL, 0 };
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz)) return FALSE;
    s.size = (ULONGLONG)sz.QuadPart;
    static const BYTE sha1_oid[] = { 0x2B, 0x0E, 0x03, 0x02, 0x1A }, sha256_oid[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
    Span oid = sha256 ? (Span){ sha256_oid, sizeof(sha256_oid) } : (Span){ sha1_oid, sizeof(sha1_oid) };
    PeLayout pl;
    LONG e = pe_layout(&s, &pl);
    BOOL ok;
    if (e == 0 || e == TRUST_E_NOSIGNATURE_) {
        if (e) pl.cert_off = (DWORD)s.size;            /* (unsigned: up to the end) */
        DWORD got = 0;
        ok = pe_digest(&s, &pl, oid, hash, &got);
    } else {
        Hash h;
        ok = hash_start(oid, &h);
        if (ok) { subj_hash(&s, &h, 0, s.size); hash_end(&h, hash); }
    }
    zfree(s.buf);
    *n = len;
    return ok;
}

WTAPI BOOL WINAPI CryptCATAdminCalcHashFromFileHandle(HANDLE f, DWORD *n, BYTE *hash, DWORD flags)
{
    (void)flags;
    return calc_hash(f, FALSE, n, hash);
}
WTAPI BOOL WINAPI CryptCATAdminCalcHashFromFileHandle2(HANDLE h, HANDLE f, DWORD *n, BYTE *hash, DWORD flags)
{
    (void)flags;
    CatAdmin *c = h;
    return calc_hash(f, c && c->magic == CAT_MAGIC && c->sha256, n, hash);
}
WTAPI HANDLE WINAPI CryptCATAdminEnumCatalogFromHash(HANDLE h, BYTE *hash, DWORD n, DWORD flags, HANDLE *prev)
{ (void)h; (void)hash; (void)n; (void)flags; (void)prev; SetLastError(ERROR_NOT_FOUND); return 0; }
WTAPI BOOL WINAPI CryptCATAdminReleaseCatalogContext(HANDLE h, HANDLE c, DWORD flags) { (void)h; (void)c; (void)flags; return TRUE; }
