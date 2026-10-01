/*
 * crypt32.dll — certificate stores.  The system stores ("ROOT", "MY",
 * "CA"...) open and are empty: programs that bring their own certificate
 * bundle (Node.js, Python, the JDK) use that, and the ones that look for
 * the system's find nothing to add.
 */
#include <windows.h>

#define CRYPT32API __declspec(dllexport)
#define CRYPT_E_NOT_FOUND_ 0x80092004L

typedef struct { DWORD magic; LONG refs; } Store;
#define STORE_MAGIC 0x53544F52                      /* "STOR" */

static HANDLE new_store(void)
{
    Store *s = HeapAlloc(GetProcessHeap(), 0, sizeof(Store));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    s->magic = STORE_MAGIC;
    s->refs = 1;
    return s;
}
static BOOL is_store(HANDLE h) { return h && ((Store *)h)->magic == STORE_MAGIC; }

CRYPT32API HANDLE WINAPI CertOpenStore(LPCSTR provider, DWORD enc, ULONG_PTR prov, DWORD flags, const void *para)
{
    (void)provider; (void)enc; (void)prov; (void)flags; (void)para;
    return new_store();
}
CRYPT32API HANDLE WINAPI CertOpenSystemStoreW(ULONG_PTR prov, LPCWSTR name) { (void)prov; (void)name; return new_store(); }
CRYPT32API HANDLE WINAPI CertOpenSystemStoreA(ULONG_PTR prov, LPCSTR name)  { (void)prov; (void)name; return new_store(); }
CRYPT32API HANDLE WINAPI CertDuplicateStore(HANDLE h)
{
    if (is_store(h)) InterlockedIncrement(&((Store *)h)->refs);
    return h;
}
CRYPT32API BOOL WINAPI CertCloseStore(HANDLE h, DWORD flags)
{
    (void)flags;
    if (!h) return TRUE;
    if (!is_store(h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!InterlockedDecrement(&((Store *)h)->refs)) { ((Store *)h)->magic = 0; HeapFree(GetProcessHeap(), 0, h); }
    return TRUE;
}

/* Certificates: the stores hold none */
CRYPT32API const void *WINAPI CertEnumCertificatesInStore(HANDLE h, const void *prev)
{
    (void)h; (void)prev;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return 0;
}
CRYPT32API const void *WINAPI CertFindCertificateInStore(HANDLE h, DWORD enc, DWORD flags, DWORD type, const void *para, const void *prev)
{
    (void)h; (void)enc; (void)flags; (void)type; (void)para; (void)prev;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return 0;
}
CRYPT32API const void *WINAPI CertDuplicateCertificateContext(const void *c) { return c; }
CRYPT32API BOOL WINAPI CertFreeCertificateContext(const void *c) { (void)c; return TRUE; }
CRYPT32API BOOL WINAPI CertGetCertificateContextProperty(const void *c, DWORD id, void *data, DWORD *n)
{
    (void)c; (void)id; (void)data; (void)n;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return FALSE;
}
CRYPT32API BOOL WINAPI CertGetEnhancedKeyUsage(const void *c, DWORD flags, void *usage, DWORD *n)
{
    (void)c; (void)flags; (void)usage; (void)n;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return FALSE;
}
CRYPT32API BOOL WINAPI CertAddEncodedCertificateToStore(HANDLE h, DWORD enc, const BYTE *data, DWORD n, DWORD disp, const void **out)
{
    (void)h; (void)enc; (void)data; (void)n; (void)disp;
    if (out) *out = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
CRYPT32API BOOL WINAPI CertGetCertificateChain(HANDLE engine, const void *c, LPFILETIME t, HANDLE store, const void *para, DWORD flags, PVOID r, const void **chain)
{
    (void)engine; (void)c; (void)t; (void)store; (void)para; (void)flags; (void)r;
    if (chain) *chain = 0;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return FALSE;
}
CRYPT32API VOID WINAPI CertFreeCertificateChain(const void *chain) { (void)chain; }

/* Base64 and binary encoding helpers */
CRYPT32API BOOL WINAPI CryptBinaryToStringA(const BYTE *b, DWORD n, DWORD flags, LPSTR out, DWORD *len)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if ((flags & 0xFFFF) != 1 /* CRYPT_STRING_BASE64 */) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    BOOL nocrlf = (flags & 0x40000000) != 0;
    DWORD body = (n + 2) / 3 * 4, need = body + (nocrlf ? 0 : 2 * ((body + 63) / 64)) + 1;
    if (!out) { *len = need; return TRUE; }
    if (*len < need) { *len = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    DWORD o = 0, col = 0;
    for (DWORD i = 0; i < n; i += 3) {
        DWORD v = (DWORD)b[i] << 16 | (i + 1 < n ? (DWORD)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
        if (!nocrlf && (col += 4) == 64) { out[o++] = '\r'; out[o++] = '\n'; col = 0; }
    }
    if (!nocrlf && col) { out[o++] = '\r'; out[o++] = '\n'; }
    out[o] = 0;
    *len = o;
    return TRUE;
}

/* Revocation lists: none in the stores; collections add nothing */
CRYPT32API const void *WINAPI CertEnumCRLsInStore(HANDLE h, const void *prev) { (void)h; (void)prev; SetLastError(CRYPT_E_NOT_FOUND_); return 0; }
CRYPT32API BOOL WINAPI CertFreeCRLContext(const void *c) { (void)c; return TRUE; }
CRYPT32API BOOL WINAPI CertAddStoreToCollection(HANDLE coll, HANDLE sib, DWORD flags, DWORD prio)
{
    (void)coll; (void)sib; (void)flags; (void)prio;
    return TRUE;
}
