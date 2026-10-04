/* authtest.exe — Authenticode: WinVerifyTrust over the signed test files
 * tools/authenticode/mktests.py makes (C:\Tests\Authenticode), signed by a
 * test certificate authority whose root this test adds to the ROOT store
 * and removes again.  Good signatures (SHA-256, SHA-1, a 32-bit program, a
 * nested signature, an expired signer saved by a PKCS #9 or an RFC 3161
 * timestamp) verify; a changed byte, a damaged signature, an untrusted
 * root, an expired signer without (or with a late) timestamp, the wrong
 * key usage, no signature and a file that is not a program each fail with
 * Windows' error.  Also the provider state (WTHelper*), CryptQueryObject
 * and the signer's name, revocation asked for while offline, the
 * catalog hash, and the Microsoft root chain policy Edge Update checks its
 * packages with (on Microsoft's own code signing CA, mspca2024.cer). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* wintrust.h / wincrypt.h, as much as this test uses */
typedef struct { DWORD cbStruct; LPCWSTR pcwszFilePath; HANDLE hFile; GUID *pgKnownSubject; } WINTRUST_FILE_INFO;
typedef struct { DWORD cbStruct, dwIndex, dwFlags, cSecondarySigs, dwVerifiedSigIndex; void *pCryptoPolicy; } WINTRUST_SIGNATURE_SETTINGS;
typedef struct {
    DWORD cbStruct;
    LPVOID pPolicyCallbackData, pSIPClientData;
    DWORD dwUIChoice, fdwRevocationChecks, dwUnionChoice;
    WINTRUST_FILE_INFO *pFile;
    DWORD dwStateAction;
    HANDLE hWVTStateData;
    WCHAR *pwszURLReference;
    DWORD dwProvFlags, dwUIContext;
    WINTRUST_SIGNATURE_SETTINGS *pSignatureSettings;
} WINTRUST_DATA;
typedef struct { DWORD cbData; BYTE *pbData; } BLOB;
typedef struct { DWORD dwCertEncodingType; BYTE *pbCertEncoded; DWORD cbCertEncoded; void *pCertInfo; HANDLE hCertStore; } CERT_CONTEXT;
typedef struct {
    DWORD cbStruct;
    const CERT_CONTEXT *pCert;
    BOOL fCommercial, fTrustedRoot, fSelfSigned, fTestCert;
    DWORD dwRevokedReason, dwConfidence, dwError;
    void *pTrustListContext;
    BOOL fTrustListSignerCert;
    void *pCtlContext;
    DWORD dwCtlError;
    BOOL fIsCyclic;
    void *pChainElement;
} CRYPT_PROVIDER_CERT;
typedef struct CRYPT_PROVIDER_SGNR {
    DWORD cbStruct;
    FILETIME sftVerifyAsOf;
    DWORD csCertChain;
    CRYPT_PROVIDER_CERT *pasCertChain;
    DWORD dwSignerType;
    void *psSigner;
    DWORD dwError, csCounterSigners;
    struct CRYPT_PROVIDER_SGNR *pasCounterSigners;
    const void *pChainContext;
} CRYPT_PROVIDER_SGNR;
typedef struct {
    DWORD dwVersion;
    BLOB Issuer, SerialNumber;
    struct { LPSTR pszObjId; BLOB Parameters; } HashAlgorithm, HashEncryptionAlgorithm;
    BLOB EncryptedHash;
    struct { DWORD cAttr; void *rgAttr; } AuthAttrs, UnauthAttrs;
} CMSG_SIGNER_INFO;

__declspec(dllimport) LONG WINAPI WinVerifyTrust(HWND, GUID *, LPVOID);
__declspec(dllimport) void *WINAPI WTHelperProvDataFromStateData(HANDLE);
__declspec(dllimport) CRYPT_PROVIDER_SGNR *WINAPI WTHelperGetProvSignerFromChain(void *, DWORD, BOOL, DWORD);
__declspec(dllimport) CRYPT_PROVIDER_CERT *WINAPI WTHelperGetProvCertFromChain(CRYPT_PROVIDER_SGNR *, DWORD);
__declspec(dllimport) BOOL WINAPI CryptCATAdminAcquireContext(HANDLE *, const GUID *, DWORD);
__declspec(dllimport) BOOL WINAPI CryptCATAdminReleaseContext(HANDLE, DWORD);
__declspec(dllimport) BOOL WINAPI CryptCATAdminCalcHashFromFileHandle(HANDLE, DWORD *, BYTE *, DWORD);
__declspec(dllimport) HANDLE WINAPI CryptCATAdminEnumCatalogFromHash(HANDLE, BYTE *, DWORD, DWORD, HANDLE *);
__declspec(dllimport) HANDLE WINAPI CertOpenSystemStoreW(ULONG_PTR, LPCWSTR);
__declspec(dllimport) BOOL WINAPI CertCloseStore(HANDLE, DWORD);
__declspec(dllimport) BOOL WINAPI CertAddEncodedCertificateToStore(HANDLE, DWORD, const BYTE *, DWORD, DWORD, const CERT_CONTEXT **);
__declspec(dllimport) const CERT_CONTEXT *WINAPI CertFindCertificateInStore(HANDLE, DWORD, DWORD, DWORD, const void *, const CERT_CONTEXT *);
__declspec(dllimport) BOOL WINAPI CertDeleteCertificateFromStore(const CERT_CONTEXT *);
__declspec(dllimport) BOOL WINAPI CertFreeCertificateContext(const CERT_CONTEXT *);
__declspec(dllimport) const CERT_CONTEXT *WINAPI CertGetSubjectCertificateFromStore(HANDLE, DWORD, void *);
__declspec(dllimport) DWORD WINAPI CertGetNameStringW(const CERT_CONTEXT *, DWORD, DWORD, void *, LPWSTR, DWORD);
__declspec(dllimport) DWORD WINAPI CertGetNameStringA(const CERT_CONTEXT *, DWORD, DWORD, void *, LPSTR, DWORD);
__declspec(dllimport) DWORD WINAPI CertNameToStrA(DWORD, BLOB *, DWORD, LPSTR, DWORD);
__declspec(dllimport) BOOL WINAPI CryptQueryObject(DWORD, const void *, DWORD, DWORD, DWORD, DWORD *, DWORD *, DWORD *, HANDLE *,
                                                   HANDLE *, const void **);
__declspec(dllimport) BOOL WINAPI CryptMsgGetParam(HANDLE, DWORD, DWORD, void *, DWORD *);
__declspec(dllimport) BOOL WINAPI CryptMsgClose(HANDLE);
__declspec(dllimport) const CERT_CONTEXT *WINAPI CertCreateCertificateContext(DWORD, const BYTE *, DWORD);
__declspec(dllimport) const CERT_CONTEXT *WINAPI CertEnumCertificatesInStore(HANDLE, const CERT_CONTEXT *);
__declspec(dllimport) BOOL WINAPI CertGetCertificateChain(HANDLE, const CERT_CONTEXT *, FILETIME *, HANDLE, const void *, DWORD, void *,
                                                          const void **);
__declspec(dllimport) VOID WINAPI CertFreeCertificateChain(const void *);
__declspec(dllimport) BOOL WINAPI CertVerifyCertificateChainPolicy(LPCSTR, const void *, void *, void *);

static GUID V2 = { 0x00AAC56B, 0xCD44, 0x11D0, { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } };
#define DIR L"C:\\Tests\\Authenticode\\"

#define TRUST_E_SUBJECT_FORM_UNKNOWN 0x800B0003
#define TRUST_E_BAD_DIGEST           0x80096010
#define TRUST_E_NOSIGNATURE          0x800B0100
#define CERT_E_EXPIRED               0x800B0101
#define CERT_E_UNTRUSTEDROOT         0x800B0109
#define CERT_E_REVOCATION_FAILURE    0x800B010E
#define CERT_E_WRONG_USAGE           0x800B0110
#define TRUST_E_PROVIDER_UNKNOWN     0x800B0001

static WCHAR path[MAX_PATH];
static const WCHAR *at(const WCHAR *name) { swprintf(path, MAX_PATH, DIR L"%ls", name); return path; }

/* WinVerifyTrust as Steam's service calls it: a file by name, no UI, no
 * revocation, no state kept */
static LONG verify_ex(const WCHAR *name, DWORD revocation, WINTRUST_SIGNATURE_SETTINGS *ss, DWORD size)
{
    WINTRUST_FILE_INFO fi = { sizeof(fi), at(name), NULL, NULL };
    WINTRUST_DATA wd = { 0 };
    wd.cbStruct = size;
    wd.dwUIChoice = 2;                                  /* WTD_UI_NONE */
    wd.fdwRevocationChecks = revocation;
    wd.dwUnionChoice = 1;                               /* WTD_CHOICE_FILE */
    wd.pFile = &fi;
    wd.pSignatureSettings = ss;
    return WinVerifyTrust(INVALID_HANDLE_VALUE, &V2, &wd);
}
static LONG verify(const WCHAR *name) { return verify_ex(name, 0, NULL, sizeof(WINTRUST_DATA) - sizeof(void *)); }

static void expect(const WCHAR *name, DWORD want)
{
    LONG got = verify(name);
    if ((DWORD)got == want) pass++;
    else { fail++; printf("FAIL: %ls: 0x%08lX, wanted 0x%08lX\n", name, (unsigned long)got, (unsigned long)want); }
}

/* CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT) on a
 * chain, with @flags; the policy's error, its element in @element */
typedef struct { DWORD cbSize, dwFlags; void *pvExtraPolicyPara; } POLICY_PARA;
typedef struct { DWORD cbSize, dwError; LONG lChainIndex, lElementIndex; void *pvExtraPolicyStatus; } POLICY_STATUS;
#define MS_ROOT_POLICY      ((LPCSTR)7)
#define MS_APPLICATION_ROOT 0x20000                     /* MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG */
static DWORD ms_root(const void *chain, DWORD flags, LONG *element)
{
    POLICY_PARA pp = { sizeof(pp), flags, NULL };
    POLICY_STATUS ps = { sizeof(ps), 0xFFFFFFFF, 0, 0, NULL };
    if (!chain || !CertVerifyCertificateChainPolicy(MS_ROOT_POLICY, chain, &pp, &ps)) return 0xFFFFFFFF;
    if (element) *element = ps.lElementIndex;
    return ps.dwError;
}

/* The chain of a certificate (the DER, or the context) to the ROOT store */
static const void *chain_of(const CERT_CONTEXT *c)
{
    struct { DWORD cbSize; struct { DWORD dwType; DWORD n; LPSTR *ids; } usage; } para = { sizeof(para), { 0, 0, NULL } };
    const void *chain = NULL;
    if (c && !CertGetCertificateChain(NULL, c, NULL, NULL, &para, 0, NULL, &chain)) chain = NULL;
    return chain;
}

static BYTE *load(const WCHAR *name, DWORD *n)
{
    HANDLE f = CreateFileW(at(name), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE *b = HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (b && ReadFile(f, b, size, &got, NULL)) b[got] = 0;
    CloseHandle(f);
    *n = got;
    return b;
}

/* The test root in the ROOT store: added, or every copy removed */
static BOOL set_test_root(BOOL trust)
{
    DWORD n = 0;
    BYTE *der = load(L"testroot.cer", &n);
    HANDLE root = CertOpenSystemStoreW(0, L"ROOT");
    BOOL ok = der && root;
    if (ok && trust) ok = CertAddEncodedCertificateToStore(root, 1, der, n, 3 /* REPLACE_EXISTING */, NULL);
    else if (ok) {
        CERT_CONTEXT want = { 1, der, n, NULL, NULL };
        const CERT_CONTEXT *c;
        while ((c = CertFindCertificateInStore(root, 1, 0, 13 << 16 /* CERT_FIND_EXISTING */, &want, NULL)) != NULL)
            CertDeleteCertificateFromStore(c);
    }
    if (root) CertCloseStore(root, 0);
    HeapFree(GetProcessHeap(), 0, der);
    return ok;
}

int main(void)
{
    /* nothing but the store decides: the test root is not trusted yet */
    set_test_root(FALSE);
    expect(L"signed.exe", CERT_E_UNTRUSTEDROOT);
    CHECK("trust the test root", set_test_root(TRUE));

    expect(L"signed.exe", 0);
    expect(L"signed-sha1.exe", 0);
    expect(L"signed32.exe", 0);
    expect(L"nested.exe", 0);
    expect(L"expired-ts.exe", 0);
    expect(L"expired-rfc3161.exe", 0);
    expect(L"tampered.exe", TRUST_E_BAD_DIGEST);
    expect(L"badsig.exe", TRUST_E_BAD_DIGEST);
    expect(L"untrusted.exe", CERT_E_UNTRUSTEDROOT);
    expect(L"expired.exe", CERT_E_EXPIRED);
    expect(L"expired-latets.exe", CERT_E_EXPIRED);
    expect(L"wrongusage.exe", CERT_E_WRONG_USAGE);
    expect(L"unsigned.exe", TRUST_E_NOSIGNATURE);
    expect(L"notpe.txt", TRUST_E_SUBJECT_FORM_UNKNOWN);
    CHECK("an unknown action", WinVerifyTrust(NULL, &(GUID){ 1 }, NULL) == (LONG)TRUST_E_PROVIDER_UNKNOWN);

    /* the full WINTRUST_DATA, revocation asked for: offline, as Windows
     * without a network */
    CHECK("revocation offline", (DWORD)verify_ex(L"signed.exe", 1 /* WTD_REVOKE_WHOLECHAIN */, NULL, sizeof(WINTRUST_DATA)) ==
                                CERT_E_REVOCATION_FAILURE);

    /* nested signatures: counted, and the second one verified */
    WINTRUST_SIGNATURE_SETTINGS ss = { sizeof(ss), 0, 2 /* WSS_GET_SECONDARY_SIG_COUNT */, 0, 0, NULL };
    CHECK("count nested", verify_ex(L"nested.exe", 0, &ss, sizeof(WINTRUST_DATA)) == 0 && ss.cSecondarySigs == 1);
    WINTRUST_SIGNATURE_SETTINGS one = { sizeof(one), 1, 1 /* WSS_VERIFY_SPECIFIC */, 0, 0, NULL };
    CHECK("verify nested", verify_ex(L"nested.exe", 0, &one, sizeof(WINTRUST_DATA)) == 0 && one.dwVerifiedSigIndex == 1);
    WINTRUST_SIGNATURE_SETTINGS none = { sizeof(none), 1, 1, 0, 0, NULL };
    CHECK("no nested", (DWORD)verify_ex(L"signed.exe", 0, &none, sizeof(WINTRUST_DATA)) == TRUST_E_NOSIGNATURE);

    /* the state the provider keeps: the signer's chain and the timestamp */
    WINTRUST_FILE_INFO fi = { sizeof(fi), at(L"expired-ts.exe"), NULL, NULL };
    WINTRUST_DATA wd = { sizeof(wd), NULL, NULL, 2, 0, 1, &fi, 1 /* WTD_STATEACTION_VERIFY */ };
    CHECK("verify with state", WinVerifyTrust(NULL, &V2, &wd) == 0 && wd.hWVTStateData);
    void *prov = WTHelperProvDataFromStateData(wd.hWVTStateData);
    CRYPT_PROVIDER_SGNR *sg = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : NULL;
    CHECK("signer", sg && sg->csCertChain == 2 && sg->csCounterSigners == 1 && sg->psSigner);
    CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : NULL;
    WCHAR name[128] = { 0 };
    if (pc) CertGetNameStringW(pc->pCert, 4 /* SIMPLE_DISPLAY */, 0, NULL, name, 128);
    CHECK("signer's name", !wcscmp(name, L"NovaOS Test Signer (expired)"));
    CRYPT_PROVIDER_CERT *top = sg ? WTHelperGetProvCertFromChain(sg, 1) : NULL;
    CHECK("trusted root", top && top->fTrustedRoot && top->fSelfSigned);
    CRYPT_PROVIDER_SGNR *ts = prov ? WTHelperGetProvSignerFromChain(prov, 0, TRUE, 0) : NULL;
    SYSTEMTIME st = { 0 };
    if (ts) FileTimeToSystemTime(&ts->sftVerifyAsOf, &st);
    CHECK("timestamp time", ts && st.wYear == 2001 && st.wMonth == 6 && st.wDay == 1 && st.wHour == 12);
    /* Edge Update's check of Microsoft's signature: the Microsoft root
     * policy refuses the test root, whatever the flags */
    CHECK("test root is not Microsoft's", sg && ms_root(sg->pChainContext, 0, NULL) == CERT_E_UNTRUSTEDROOT &&
                                          ms_root(sg->pChainContext, MS_APPLICATION_ROOT, NULL) == CERT_E_UNTRUSTEDROOT);
    wd.dwStateAction = 2;                               /* WTD_STATEACTION_CLOSE */
    CHECK("close state", WinVerifyTrust(NULL, &V2, &wd) == 0);

    /* CryptQueryObject: the embedded signature, its signer and names */
    DWORD enc = 0, ctype = 0, ftype = 0;
    HANDLE store = 0, msg = 0;
    CHECK("query object", CryptQueryObject(1, at(L"signed.exe"), 1 << 10 /* PKCS7_SIGNED_EMBED */, 2, 0, &enc, &ctype, &ftype,
                                           &store, &msg, NULL) && ctype == 10 && msg && store);
    DWORD sl = 0;
    CMSG_SIGNER_INFO *si = NULL;
    if (msg && CryptMsgGetParam(msg, 6 /* CMSG_SIGNER_INFO_PARAM */, 0, NULL, &sl)) {
        si = HeapAlloc(GetProcessHeap(), 0, sl);
        if (!CryptMsgGetParam(msg, 6, 0, si, &sl)) si = NULL;
    }
    CHECK("signer info", si && !strcmp(si->HashAlgorithm.pszObjId, "2.16.840.1.101.3.4.2.1") && si->EncryptedHash.cbData == 256 &&
                         si->SerialNumber.cbData == 1 && si->SerialNumber.pbData[0] == 2);
    struct { DWORD v; BLOB serial; struct { LPSTR o; BLOB p; } alg; BLOB issuer; BYTE rest[128]; } id = { 0 };
    if (si) { id.issuer = si->Issuer; id.serial = si->SerialNumber; }
    const CERT_CONTEXT *cert = si ? CertGetSubjectCertificateFromStore(store, 0x10001, &id) : NULL;
    char an[128] = { 0 }, issuer[256] = { 0 };
    if (cert) CertGetNameStringA(cert, 4, 1 /* CERT_NAME_ISSUER_FLAG */, NULL, an, 128);
    CHECK("issuer's name", !strcmp(an, "NovaOS Test Root"));
    if (si) CertNameToStrA(1, &si->Issuer, 3 /* X500 */, issuer, 256);
    CHECK("issuer as text", !strcmp(issuer, "C=US, O=NovaOS, CN=NovaOS Test Root"));
    if (cert) CertFreeCertificateContext(cert);
    if (store) CertCloseStore(store, 0);
    if (msg) CryptMsgClose(msg);

    /* the catalog hash (a PE's Authenticode SHA-1); no catalogs exist */
    DWORD n = 0;
    BYTE *want = load(L"signed.sha1", &n);
    HANDLE f = CreateFileW(at(L"signed.exe"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BYTE hash[20];
    DWORD hl = sizeof(hash);
    char hex[41] = { 0 };
    if (CryptCATAdminCalcHashFromFileHandle(f, &hl, hash, 0))
        for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02X", hash[i]);
    CHECK("catalog hash", want && n == 40 && !memcmp(hex, want, 40));
    CloseHandle(f);
    HANDLE admin = 0;
    CHECK("catalog admin", CryptCATAdminAcquireContext(&admin, NULL, 0) && !CryptCATAdminEnumCatalogFromHash(admin, hash, 20, 0, NULL));
    CryptCATAdminReleaseContext(admin, 0);

    /* the Microsoft root policy on Microsoft's own chains: the code signing
     * CA 2024 chains to the application root (Root Certificate Authority
     * 2011), which counts only with its flag; the 2010 root always counts */
    DWORD pn = 0;
    BYTE *pca = load(L"mspca2024.cer", &pn);
    const CERT_CONTEXT *pcac = pca ? CertCreateCertificateContext(1, pca, pn) : NULL;
    const void *pch = chain_of(pcac);
    LONG el = -1;
    CHECK("Microsoft application root needs its flag", ms_root(pch, 0, &el) == CERT_E_UNTRUSTEDROOT && el == 1);
    CHECK("Microsoft application root", ms_root(pch, MS_APPLICATION_ROOT, &el) == 0 && el == -1);
    if (pch) CertFreeCertificateChain(pch);
    if (pcac) CertFreeCertificateContext(pcac);
    HeapFree(GetProcessHeap(), 0, pca);
    HANDLE roots = CertOpenSystemStoreW(0, L"ROOT");
    const CERT_CONTEXT *rc = NULL;
    WCHAR rn[128];
    while ((rc = CertEnumCertificatesInStore(roots, rc)) != NULL) {
        rn[0] = 0;
        CertGetNameStringW(rc, 4, 0, NULL, rn, 128);
        if (!wcscmp(rn, L"Microsoft Root Certificate Authority 2010")) break;
    }
    const void *rch = chain_of(rc);
    CHECK("Microsoft product root", rc && ms_root(rch, 0, NULL) == 0);
    if (rch) CertFreeCertificateChain(rch);
    if (rc) CertFreeCertificateContext(rc);
    if (roots) CertCloseStore(roots, 0);

    /* trust taken back */
    CHECK("remove the test root", set_test_root(FALSE));
    expect(L"signed.exe", CERT_E_UNTRUSTEDROOT);

    printf("authtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
