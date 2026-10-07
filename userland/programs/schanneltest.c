/* schanneltest.exe — what Qt's Schannel TLS backend (GOG Galaxy's) and
 * Chromium's certificate store reader (Qt WebEngine's) ask of Windows.
 *
 *   schanneltest stores           crypt32: a certificate found through a
 *                                 collection that holds a collection (the
 *                                 way Chromium's TrustStoreWin looks up
 *                                 issuers), each store's certificates once;
 *                                 the extended key usages of a root (none:
 *                                 good for every usage, as Chromium asks
 *                                 before trusting it); and wininet's
 *                                 InternetGetConnectedState
 *                                 (prints "online" or "offline")
 *   schanneltest HOST PORT        a TLS client over SSPI with manual
 *                                 certificate checks, as Qt does: the flags
 *                                 InitializeSecurityContext returns, the
 *                                 cipher and connection attributes, the
 *                                 server's certificate (with its chain's
 *                                 store), CertGetCertificateChain on it, and
 *                                 an HTTP/1.1 request through Encrypt/
 *                                 DecryptMessage (tools/h2server.js's /hello) */
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* wincrypt.h, sspi.h and schannel.h, as much as this test uses */
typedef struct { DWORD cbData; BYTE *pbData; } BLOB_;
typedef struct {
    DWORD dwVersion; BLOB_ SerialNumber; LPSTR pszObjId; BLOB_ Parameters; BLOB_ Issuer;
    FILETIME NotBefore, NotAfter; BLOB_ Subject;
} CERT_INFO_;
typedef struct { DWORD dwCertEncodingType; BYTE *pbCertEncoded; DWORD cbCertEncoded; CERT_INFO_ *pCertInfo; HANDLE hCertStore; } CERT_CONTEXT_;
typedef struct { DWORD dwErrorStatus, dwInfoStatus; } TRUST_STATUS_;
typedef struct { DWORD cbSize; TRUST_STATUS_ TrustStatus; DWORD cElement; void **rgpElement; } SIMPLE_CHAIN_;
typedef struct { DWORD cbSize; TRUST_STATUS_ TrustStatus; DWORD cChain; SIMPLE_CHAIN_ **rgpChain; } CHAIN_CONTEXT_;
typedef struct { DWORD dwType; DWORD cUsageIdentifier; LPSTR *rgpszUsageIdentifier; } USAGE_MATCH_;
typedef struct { DWORD cbSize; USAGE_MATCH_ RequestedUsage; } CHAIN_PARA_;

#define X509_ASN_ENCODING_         1
#define CERT_STORE_PROV_MEMORY_    ((LPCSTR)2)
#define CERT_STORE_PROV_SYSTEM_W_  ((LPCSTR)10)
#define CERT_STORE_PROV_COLLECTION_ ((LPCSTR)11)
#define CERT_SYSTEM_STORE_CURRENT_USER_ 0x00010000
#define CERT_STORE_READONLY_FLAG_  0x00008000
#define CERT_STORE_ADD_ALWAYS_     4
#define CERT_FIND_SUBJECT_NAME_    (7 << 16 | 2)
#define TRUST_IS_UNTRUSTED_ROOT    0x00000020
#define CRYPT_E_NOT_FOUND_         0x80092004

__declspec(dllimport) HANDLE WINAPI CertOpenStore(LPCSTR, DWORD, ULONG_PTR, DWORD, const void *);
__declspec(dllimport) BOOL WINAPI CertCloseStore(HANDLE, DWORD);
__declspec(dllimport) BOOL WINAPI CertAddStoreToCollection(HANDLE, HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL WINAPI CertAddCertificateContextToStore(HANDLE, const CERT_CONTEXT_ *, DWORD, const CERT_CONTEXT_ **);
__declspec(dllimport) const CERT_CONTEXT_ *WINAPI CertEnumCertificatesInStore(HANDLE, const CERT_CONTEXT_ *);
__declspec(dllimport) const CERT_CONTEXT_ *WINAPI CertFindCertificateInStore(HANDLE, DWORD, DWORD, DWORD, const void *, const CERT_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertFreeCertificateContext(const CERT_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertGetCertificateChain(HANDLE, const CERT_CONTEXT_ *, FILETIME *, HANDLE, const CHAIN_PARA_ *,
                                                          DWORD, void *, const CHAIN_CONTEXT_ **);
__declspec(dllimport) void WINAPI CertFreeCertificateChain(const CHAIN_CONTEXT_ *);
__declspec(dllimport) BOOL WINAPI CertGetEnhancedKeyUsage(const CERT_CONTEXT_ *, DWORD, void *, DWORD *);
__declspec(dllimport) BOOL WINAPI InternetGetConnectedState(DWORD *, DWORD);

typedef LONG SECURITY_STATUS;
typedef struct { ULONG_PTR dwLower, dwUpper; } SecHandle;
typedef struct { ULONG cbBuffer, BufferType; void *pvBuffer; } SecBuffer;
typedef struct { ULONG ulVersion, cBuffers; SecBuffer *pBuffers; } SecBufferDesc;
typedef struct { ULONG cbHeader, cbTrailer, cbMaximumMessage, cBuffers, cbBlockSize; } StreamSizes;
typedef struct { DWORD dwProtocol, aiCipher, dwCipherStrength, aiHash, dwHashStrength, aiExch, dwExchStrength; } ConnInfo;
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
typedef struct {
    DWORD dwVersion, cCreds; void *paCred, *hRootStore; DWORD cMappers; void *aphMappers;
    DWORD cSupportedAlgs; void *palgSupportedAlgs; DWORD grbitEnabledProtocols,
    dwMinimumCipherStrength, dwMaximumCipherStrength, dwSessionLifespan, dwFlags, dwCredFormat;
} SchannelCred;

__declspec(dllimport) SECURITY_STATUS WINAPI AcquireCredentialsHandleW(WCHAR *, WCHAR *, ULONG, void *, void *, void *, void *,
                                                                       SecHandle *, LARGE_INTEGER *);
__declspec(dllimport) SECURITY_STATUS WINAPI InitializeSecurityContextW(SecHandle *, SecHandle *, WCHAR *, ULONG, ULONG, ULONG,
                                                                        SecBufferDesc *, ULONG, SecHandle *, SecBufferDesc *,
                                                                        ULONG *, LARGE_INTEGER *);
__declspec(dllimport) SECURITY_STATUS WINAPI QueryContextAttributesW(SecHandle *, ULONG, void *);
__declspec(dllimport) SECURITY_STATUS WINAPI EncryptMessage(SecHandle *, ULONG, SecBufferDesc *, ULONG);
__declspec(dllimport) SECURITY_STATUS WINAPI DecryptMessage(SecHandle *, SecBufferDesc *, ULONG, ULONG *);
__declspec(dllimport) SECURITY_STATUS WINAPI FreeContextBuffer(void *);
__declspec(dllimport) SECURITY_STATUS WINAPI DeleteSecurityContext(SecHandle *);
__declspec(dllimport) SECURITY_STATUS WINAPI FreeCredentialsHandle(SecHandle *);

#define SEC_E_OK                0
#define SEC_I_CONTINUE_NEEDED   0x00090312
#define SEC_E_INCOMPLETE_MESSAGE ((SECURITY_STATUS)0x80090318)
#define SECBUFFER_EMPTY 0
#define SECBUFFER_DATA 1
#define SECBUFFER_TOKEN 2
#define SECBUFFER_EXTRA 5
#define SECBUFFER_STREAM_TRAILER 6
#define SECBUFFER_STREAM_HEADER 7
#define ISC_REQ_MUTUAL_AUTH            0x00000002
#define ISC_REQ_REPLAY_DETECT          0x00000004
#define ISC_REQ_SEQUENCE_DETECT        0x00000008
#define ISC_REQ_CONFIDENTIALITY        0x00000010
#define ISC_REQ_ALLOCATE_MEMORY        0x00000100
#define ISC_REQ_STREAM                 0x00008000
#define ISC_REQ_MANUAL_CRED_VALIDATION 0x00080000
#define SECPKG_ATTR_STREAM_SIZES        4
#define SECPKG_ATTR_REMOTE_CERT_CONTEXT 0x53
#define SECPKG_ATTR_CONNECTION_INFO     0x5a
#define SECPKG_ATTR_CIPHER_INFO         0x64
#define SCH_CRED_MANUAL_CRED_VALIDATION 0x00000008
#define SCH_CRED_NO_DEFAULT_CREDS       0x00000010

static int stores(void)
{
    /* a certificate (any trusted root) in a memory store, inside a
     * collection, inside another collection */
    HANDLE root = CertOpenStore(CERT_STORE_PROV_SYSTEM_W_, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER_ | CERT_STORE_READONLY_FLAG_, L"ROOT");
    CHECK("ROOT opens", root != NULL);
    const CERT_CONTEXT_ *any = root ? CertEnumCertificatesInStore(root, NULL) : NULL;
    CHECK("ROOT has a certificate", any != NULL);
    if (!any) return 1;
    HANDLE mem = CertOpenStore(CERT_STORE_PROV_MEMORY_, X509_ASN_ENCODING_, 0, 0, NULL);
    HANDLE inner = CertOpenStore(CERT_STORE_PROV_COLLECTION_, 0, 0, 0, NULL);
    HANDLE outer = CertOpenStore(CERT_STORE_PROV_COLLECTION_, 0, 0, 0, NULL);
    HANDLE empty = CertOpenStore(CERT_STORE_PROV_MEMORY_, X509_ASN_ENCODING_, 0, 0, NULL);
    CHECK("stores open", mem && inner && outer && empty);
    CHECK("add to memory store", CertAddCertificateContextToStore(mem, any, CERT_STORE_ADD_ALWAYS_, NULL));
    CHECK("collection in a collection", CertAddStoreToCollection(inner, mem, 0, 0) &&
                                        CertAddStoreToCollection(outer, empty, 0, 0) &&
                                        CertAddStoreToCollection(outer, inner, 0, 0));
    /* the same store a second time, through the outer collection itself */
    CHECK("a store twice", CertAddStoreToCollection(outer, mem, 0, 0));
    const CERT_CONTEXT_ *hit = CertFindCertificateInStore(outer, X509_ASN_ENCODING_, 0, CERT_FIND_SUBJECT_NAME_,
                                                          &any->pCertInfo->Subject, NULL);
    CHECK("found through the inner collection", hit && hit->cbCertEncoded == any->cbCertEncoded &&
                                                 !memcmp(hit->pbCertEncoded, any->pbCertEncoded, any->cbCertEncoded));
    if (hit) CertFreeCertificateContext(hit);
    int n = 0;
    for (const CERT_CONTEXT_ *c = CertEnumCertificatesInStore(outer, NULL); c && n < 10; c = CertEnumCertificatesInStore(outer, c)) n++;
    CHECK("enumerated once", n == 1);
    CertCloseStore(outer, 0);
    CertCloseStore(inner, 0);
    CertCloseStore(empty, 0);
    CertCloseStore(mem, 0);

    /* a root without an extended key usage extension is good for every
     * usage: TRUE, none listed and CRYPT_E_NOT_FOUND (Chromium's TrustStoreWin
     * trusts a Windows root for servers only then) */
    DWORD size = 0;
    CHECK("usage size", CertGetEnhancedKeyUsage(any, 0, NULL, &size) && size >= 2 * sizeof(void *));
    struct { DWORD c; LPSTR *rg; } *usage = malloc(size + 1);
    SetLastError(0);
    BOOL got = usage && CertGetEnhancedKeyUsage(any, 0, usage, &size);
    CHECK("usage", got);
    if (got && !usage->c) CHECK("no usages: every usage", GetLastError() == CRYPT_E_NOT_FOUND_);
    else if (got) CHECK("usage OIDs", usage->rg && usage->rg[0] && !strncmp(usage->rg[0], "1.3.6.1.", 8));
    DWORD small = 1;
    CHECK("short buffer", !CertGetEnhancedKeyUsage(any, 0, usage, &small) && GetLastError() == ERROR_MORE_DATA && small == size);
    free(usage);
    CertFreeCertificateContext(any);
    CertCloseStore(root, 0);

    DWORD flags = 0xFFFF;
    BOOL up = InternetGetConnectedState(&flags, 0);
    CHECK("connected state flags", up ? (flags & 0x42) == 0x42 : (flags & 0x20) != 0);
    printf("network: %s (flags 0x%lx)\n", up ? "online" : "offline", flags);
    return 0;
}

static SOCKET sock;
static int send_all(const void *p, int n)
{
    for (int o = 0; o < n;) {
        int k = send(sock, (const char *)p + o, n - o, 0);
        if (k <= 0) return 0;
        o += k;
    }
    return 1;
}

static int connect_test(const char *host, int port)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr(host);
    CHECK("connect", connect(sock, (struct sockaddr *)&a, sizeof(a)) == 0);

    SchannelCred sc = { 0 };
    sc.dwVersion = 4;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    SecHandle cred, ctx = { 0 };
    CHECK("credentials", AcquireCredentialsHandleW(NULL, L"Microsoft Unified Security Protocol Provider", 2, NULL, &sc,
                                                   NULL, NULL, &cred, NULL) == SEC_E_OK);
    ULONG req = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_MUTUAL_AUTH, attrs = 0;
    static BYTE in[65536];
    int have = 0, first = 1;
    SECURITY_STATUS st;
    for (int rounds = 0; rounds < 40; rounds++) {
        SecBuffer ib[2] = { { (ULONG)have, SECBUFFER_TOKEN, in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc id = { 0, 2, ib };
        SecBuffer ob[1] = { { 0, SECBUFFER_TOKEN, NULL } };
        SecBufferDesc od = { 0, 1, ob };
        st = InitializeSecurityContextW(&cred, first ? NULL : &ctx, L"novaos-test", req, 0, 0, first ? NULL : &id, 0,
                                        &ctx, &od, &attrs, NULL);
        first = 0;
        if (ob[0].pvBuffer && ob[0].cbBuffer) send_all(ob[0].pvBuffer, (int)ob[0].cbBuffer);
        if (ob[0].pvBuffer) FreeContextBuffer(ob[0].pvBuffer);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {           /* more of the record first */
            int k = recv(sock, (char *)in + have, (int)sizeof(in) - have, 0);
            if (k <= 0) break;
            have += k;
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) break;
        if (ib[1].BufferType == SECBUFFER_EXTRA) {     /* what it did not read */
            memmove(in, in + have - ib[1].cbBuffer, ib[1].cbBuffer);
            have = (int)ib[1].cbBuffer;
        } else have = 0;
        if (st == SEC_E_OK) break;
        if (!have) {
            int k = recv(sock, (char *)in, (int)sizeof(in), 0);
            if (k <= 0) break;
            have = k;
        }
    }
    CHECK("handshake", st == SEC_E_OK);
    printf("returned flags 0x%lx\n", attrs);
    CHECK("manual validation flag back", attrs & ISC_REQ_MANUAL_CRED_VALIDATION);
    CHECK("mutual authentication flag back", attrs & ISC_REQ_MUTUAL_AUTH);
    CHECK("stream flags back", (attrs & (ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM)) == (ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM));

    StreamSizes sz = { 0 };
    CHECK("stream sizes", QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz) == SEC_E_OK && sz.cbMaximumMessage);
    CipherInfo ci;
    memset(&ci, 0, sizeof(ci));
    CHECK("cipher info", QueryContextAttributesW(&ctx, SECPKG_ATTR_CIPHER_INFO, &ci) == SEC_E_OK);
    CHECK("cipher info contents", ci.dwVersion == 1 && (ci.dwProtocol == 0x0303 || ci.dwProtocol == 0x0304) &&
                                  ci.dwCipherSuite && !wcsncmp(ci.szCipherSuite, L"TLS_", 4) && ci.dwCipherLen >= 128);
    printf("cipher suite %ls (0x%04lx), protocol 0x%lx\n", ci.szCipherSuite, ci.dwCipherSuite, ci.dwProtocol);
    ConnInfo conn;
    CHECK("connection info", QueryContextAttributesW(&ctx, SECPKG_ATTR_CONNECTION_INFO, &conn) == SEC_E_OK);

    const CERT_CONTEXT_ *cert = NULL;
    CHECK("server certificate", QueryContextAttributesW(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) == SEC_E_OK && cert);
    if (cert) {
        CHECK("certificate in a store", cert->hCertStore && CertEnumCertificatesInStore(cert->hCertStore, NULL));
        /* Qt's check: the system roots and the certificate's own store */
        HANDLE coll = CertOpenStore(CERT_STORE_PROV_COLLECTION_, X509_ASN_ENCODING_, 0, 0, NULL);
        HANDLE root = CertOpenStore(CERT_STORE_PROV_SYSTEM_W_, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER_ | CERT_STORE_READONLY_FLAG_, L"ROOT");
        CertAddStoreToCollection(coll, root, 0, 1);
        CertAddStoreToCollection(coll, cert->hCertStore, 0, 0);
        LPSTR oid = "1.3.6.1.5.5.7.3.1";
        CHAIN_PARA_ para = { sizeof(para), { 0, 1, &oid } };
        const CHAIN_CONTEXT_ *chain = NULL;
        CHECK("chain", CertGetCertificateChain(NULL, cert, NULL, coll, &para, 0, NULL, &chain) && chain && chain->cChain == 1);
        /* the test server's certificate is self-signed: not a trusted root */
        if (chain) {
            CHECK("self-signed server: untrusted root", chain->TrustStatus.dwErrorStatus & TRUST_IS_UNTRUSTED_ROOT);
            CertFreeCertificateChain(chain);
        }
        CertCloseStore(coll, 0);
        CertCloseStore(root, 0);
        CertFreeCertificateContext(cert);
    }

    /* a request and its answer */
    static char msg[16384 + 1024];
    const char *get = "GET /hello HTTP/1.1\r\nHost: novaos-test\r\nConnection: close\r\n\r\n";
    int glen = (int)strlen(get);
    memcpy(msg + sz.cbHeader, get, glen);
    SecBuffer eb[4] = { { sz.cbHeader, SECBUFFER_STREAM_HEADER, msg }, { (ULONG)glen, SECBUFFER_DATA, msg + sz.cbHeader },
                        { sz.cbTrailer, SECBUFFER_STREAM_TRAILER, msg + sz.cbHeader + glen }, { 0, SECBUFFER_EMPTY, NULL } };
    SecBufferDesc ed = { 0, 4, eb };
    CHECK("encrypt", EncryptMessage(&ctx, 0, &ed, 0) == SEC_E_OK);
    send_all(msg, (int)(eb[0].cbBuffer + eb[1].cbBuffer + eb[2].cbBuffer));
    char text[4096] = "";
    int tl = 0;
    for (int rounds = 0; rounds < 50 && !strstr(text, "</html>");) {
        SecBuffer db[4] = { { (ULONG)have, SECBUFFER_DATA, in }, { 0, SECBUFFER_EMPTY, NULL },
                            { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc dd = { 0, 4, db };
        st = have ? DecryptMessage(&ctx, &dd, 0, NULL) : SEC_E_INCOMPLETE_MESSAGE;
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            int k = recv(sock, (char *)in + have, (int)sizeof(in) - have, 0);
            if (k <= 0) break;
            have += k;
            rounds++;
            continue;
        }
        if (st != SEC_E_OK) break;
        int extra = 0;
        for (int i = 0; i < 4; i++) {
            if (db[i].BufferType == SECBUFFER_DATA && db[i].cbBuffer && tl + (int)db[i].cbBuffer < (int)sizeof(text) - 1) {
                memcpy(text + tl, db[i].pvBuffer, db[i].cbBuffer);
                tl += (int)db[i].cbBuffer;
                text[tl] = 0;
            }
            if (db[i].BufferType == SECBUFFER_EXTRA) extra = (int)db[i].cbBuffer;
        }
        if (extra) memmove(in, in + have - extra, extra);
        have = extra;
    }
    CHECK("answer", !strncmp(text, "HTTP/1.1 200", 12) && strstr(text, "Hello over HTTP/1.1"));
    DeleteSecurityContext(&ctx);
    FreeCredentialsHandle(&cred);
    closesocket(sock);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "stores")) stores();
    else if (argc >= 3) connect_test(argv[1], atoi(argv[2]));
    else { printf("usage: schanneltest stores | schanneltest HOST PORT\n"); return 2; }
    printf("schanneltest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
