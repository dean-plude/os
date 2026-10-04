/*
 * oidinfo.c — CryptFindOIDInfo: what crypt32 knows about an object
 * identifier (its name, group, CryptoAPI ALG_ID and CNG algorithm), found
 * by OID, name, ALG_ID, signature's (hash, public key) pair or CNG name.
 * The table holds the algorithms, name attributes, extensions and key
 * usages certificates and TLS use; anything else is not found.
 */
#include <windows.h>

#define CRYPT32API __declspec(dllexport)

typedef struct {
    DWORD cbSize;
    LPCSTR pszOID;
    LPCWSTR pwszName;
    DWORD dwGroupId;
    DWORD dwValue;                      /* ALG_ID, or a length */
    struct { DWORD cbData; BYTE *pbData; } ExtraInfo;
    LPCWSTR pwszCNGAlgid;
    LPCWSTR pwszCNGExtraAlgid;
} OID_INFO;

enum { HASH = 1, ENCRYPT = 2, PUBKEY = 3, SIGN = 4, RDN = 5, EXT = 6, EKU = 7 };
enum { BY_OID = 1, BY_NAME = 2, BY_ALGID = 3, BY_SIGN = 4, BY_CNG_ALGID = 5, BY_CNG_SIGN = 6 };
#define KEY_GROUP_MASK 0x0FFFFFFF       /* the key type's high bits carry flags */

#define CALG_MD5_      0x8003
#define CALG_SHA1_     0x8004
#define CALG_SHA_256_  0x800c
#define CALG_SHA_384_  0x800d
#define CALG_SHA_512_  0x800e
#define CALG_RSA_SIGN_ 0x2400
#define CALG_RSA_KEYX_ 0xa400
#define CALG_ECDSA_    0x2203
#define CALG_ECDH_     0xaa05
#define CALG_OID_INFO_PARAMETERS 0xFFFFFFFE
#define CALG_AES_128_  0x660e
#define CALG_AES_256_  0x6610

/* a signature's ExtraInfo: its public key's ALG_ID and flags */
static DWORD rsa_sign[] = { CALG_RSA_SIGN_, 0 };
static DWORD rsa_pss[] = { CALG_RSA_SIGN_, 0 };
static DWORD ecdsa_sign[] = { CALG_OID_INFO_PARAMETERS, 0 };

#define SIGN_EXTRA(a) { sizeof(a), (BYTE *)(a) }
static OID_INFO g_oids[] = {
    { sizeof(OID_INFO), "1.2.840.113549.2.5", L"md5", HASH, CALG_MD5_, { 0, 0 }, L"MD5", 0 },
    { sizeof(OID_INFO), "1.3.14.3.2.26", L"sha1", HASH, CALG_SHA1_, { 0, 0 }, L"SHA1", 0 },
    { sizeof(OID_INFO), "2.16.840.1.101.3.4.2.1", L"sha256", HASH, CALG_SHA_256_, { 0, 0 }, L"SHA256", 0 },
    { sizeof(OID_INFO), "2.16.840.1.101.3.4.2.2", L"sha384", HASH, CALG_SHA_384_, { 0, 0 }, L"SHA384", 0 },
    { sizeof(OID_INFO), "2.16.840.1.101.3.4.2.3", L"sha512", HASH, CALG_SHA_512_, { 0, 0 }, L"SHA512", 0 },
    { sizeof(OID_INFO), "2.16.840.1.101.3.4.1.2", L"aes128", ENCRYPT, CALG_AES_128_, { 0, 0 }, L"AES", 0 },
    { sizeof(OID_INFO), "2.16.840.1.101.3.4.1.42", L"aes256", ENCRYPT, CALG_AES_256_, { 0, 0 }, L"AES", 0 },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.1", L"RSA", PUBKEY, CALG_RSA_KEYX_, { 0, 0 }, L"RSA", 0 },
    { sizeof(OID_INFO), "1.2.840.10045.2.1", L"ECC", PUBKEY, CALG_OID_INFO_PARAMETERS, { 0, 0 }, L"CryptOIDInfoECCParameters", 0 },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.4", L"md5RSA", SIGN, CALG_MD5_, SIGN_EXTRA(rsa_sign), L"MD5", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.5", L"sha1RSA", SIGN, CALG_SHA1_, SIGN_EXTRA(rsa_sign), L"SHA1", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.11", L"sha256RSA", SIGN, CALG_SHA_256_, SIGN_EXTRA(rsa_sign), L"SHA256", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.12", L"sha384RSA", SIGN, CALG_SHA_384_, SIGN_EXTRA(rsa_sign), L"SHA384", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.13", L"sha512RSA", SIGN, CALG_SHA_512_, SIGN_EXTRA(rsa_sign), L"SHA512", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.113549.1.1.10", L"RSASSA-PSS", SIGN, CALG_OID_INFO_PARAMETERS, SIGN_EXTRA(rsa_pss), L"CryptOIDInfoParameters", L"RSA" },
    { sizeof(OID_INFO), "1.2.840.10045.4.1", L"sha1ECDSA", SIGN, CALG_SHA1_, SIGN_EXTRA(ecdsa_sign), L"SHA1", L"CryptOIDInfoECCParameters" },
    { sizeof(OID_INFO), "1.2.840.10045.4.3.2", L"sha256ECDSA", SIGN, CALG_SHA_256_, SIGN_EXTRA(ecdsa_sign), L"SHA256", L"CryptOIDInfoECCParameters" },
    { sizeof(OID_INFO), "1.2.840.10045.4.3.3", L"sha384ECDSA", SIGN, CALG_SHA_384_, SIGN_EXTRA(ecdsa_sign), L"SHA384", L"CryptOIDInfoECCParameters" },
    { sizeof(OID_INFO), "1.2.840.10045.4.3.4", L"sha512ECDSA", SIGN, CALG_SHA_512_, SIGN_EXTRA(ecdsa_sign), L"SHA512", L"CryptOIDInfoECCParameters" },
    { sizeof(OID_INFO), "2.5.4.3", L"CN", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.4.6", L"C", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.4.7", L"L", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.4.8", L"S", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.4.10", L"O", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.4.11", L"OU", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.2.840.113549.1.9.1", L"E", RDN, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.15", L"Key Usage", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.17", L"Subject Alternative Name", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.19", L"Basic Constraints", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.31", L"CRL Distribution Points", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.32", L"Certificate Policies", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "2.5.29.37", L"Enhanced Key Usage", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.1.1", L"Authority Information Access", EXT, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.3.1", L"Server Authentication", EKU, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.3.2", L"Client Authentication", EKU, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.3.3", L"Code Signing", EKU, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.3.4", L"Secure Email", EKU, 0, { 0, 0 }, 0, 0 },
    { sizeof(OID_INFO), "1.3.6.1.5.5.7.3.8", L"Time Stamping", EKU, 0, { 0, 0 }, 0, 0 },
};

static int same_a(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static int same_w_nocase(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return 0;
    for (;; a++, b++) {
        WCHAR x = *a >= 'A' && *a <= 'Z' ? (WCHAR)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (WCHAR)(*b + 32) : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

CRYPT32API const OID_INFO *WINAPI CryptFindOIDInfo(DWORD keytype, void *key, DWORD group)
{
    group &= KEY_GROUP_MASK & 0xFFFF;
    if (!key) return 0;
    for (unsigned i = 0; i < sizeof(g_oids) / sizeof(g_oids[0]); i++) {
        const OID_INFO *o = &g_oids[i];
        if (group && o->dwGroupId != group) continue;
        switch (keytype & 0xFF) {
        case BY_OID: if (same_a(o->pszOID, key)) return o; break;
        case BY_NAME: if (same_w_nocase(o->pwszName, key)) return o; break;
        case BY_ALGID: if (o->dwGroupId != SIGN && o->dwValue && o->dwValue == *(DWORD *)key) return o; break;
        case BY_SIGN: {
            const DWORD *pair = key;                /* (hash ALG_ID, public key ALG_ID) */
            if (o->dwGroupId == SIGN && o->dwValue == pair[0] && *(DWORD *)o->ExtraInfo.pbData == pair[1]) return o;
            break;
        }
        case BY_CNG_ALGID: if (o->dwGroupId != SIGN && same_w_nocase(o->pwszCNGAlgid, key)) return o; break;
        case BY_CNG_SIGN: {
            LPCWSTR *pair = key;                    /* (hash, public key) CNG names */
            if (o->dwGroupId == SIGN && same_w_nocase(o->pwszCNGAlgid, pair[0]) &&
                same_w_nocase(o->pwszCNGExtraAlgid, pair[1])) return o;
            break;
        }
        }
    }
    return 0;
}
