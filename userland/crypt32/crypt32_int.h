/*
 * crypt32_int.h — what crypt32's sources share among themselves: the
 * certificate context (certs.c) the message code (msg.c) looks inside, and
 * the message's certificates the message stores (CERT_STORE_PROV_MSG,
 * CERT_STORE_PROV_PKCS7) are filled from.
 */
#ifndef CRYPT32_INT_H
#define CRYPT32_INT_H

#include <windows.h>

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

/* msg.c: a message handle's certificates (DER), for the message stores */
DWORD crypt32_msg_cert_count(HANDLE msg);
const BYTE *crypt32_msg_cert(HANDLE msg, DWORD i, DWORD *n);
/* msg.c: decode a PKCS #7 blob into a message handle (CryptMsgClose it) */
HANDLE crypt32_msg_from_blob(const BYTE *b, DWORD n);

/* certs.c: start Mbed TLS (its keys live in PSA, which must be up first) */
void crypt32_init(void);
__declspec(dllexport) BOOL WINAPI CryptMsgClose(HANDLE msg);
__declspec(dllexport) const void *WINAPI CertCreateCertificateContext(DWORD enc, const BYTE *b, DWORD n);
__declspec(dllexport) BOOL WINAPI CertFreeCertificateContext(const void *c);
__declspec(dllexport) HANDLE WINAPI CertOpenStore(LPCSTR provider, DWORD enc, ULONG_PTR prov, DWORD flags, const void *para);
__declspec(dllexport) BOOL WINAPI CertAddCertificateContextToStore(HANDLE h, const void *cv, DWORD disp, const void **out);
__declspec(dllexport) BOOL WINAPI CertAddEncodedCertificateToStore(HANDLE h, DWORD enc, const BYTE *data, DWORD n, DWORD disp, const void **out);

/* certs.c: a memory store, and adding DER certificates to it */
HANDLE crypt32_memory_store(void);
BOOL crypt32_store_add_der(HANDLE store, const BYTE *der, DWORD n, const void **out);

#endif
