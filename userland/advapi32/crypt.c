/*
 * crypt.c — advapi32's random numbers (RtlGenRandom = SystemFunction036,
 * from the kernel's entropy pool) and the CryptoAPI's providers and
 * hashes (MD5, SHA-1, SHA-2).  Keys and ciphers are not provided.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"
#include "../common/hash.h"

WINADVAPI BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG n)
{
    return NT_SUCCESS(NtNovaGetRandom(buf, n));
}

#define PROV_MAGIC 0x4E4F5641u
typedef struct { DWORD magic; } Prov;
typedef struct { DWORD magic; DWORD alg; NovaHash h; BYTE digest[64]; BOOL done; } Hash;

WINADVAPI BOOL WINAPI CryptAcquireContextW(HCRYPTPROV *p, LPCWSTR c, LPCWSTR prov, DWORD type, DWORD flags)
{
    (void)c; (void)prov; (void)type;
    if (flags & 0x10 /* CRYPT_DELETEKEYSET */) { *p = 0; return TRUE; }
    Prov *x = LocalAlloc(0, sizeof(*x));
    if (!x) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    x->magic = PROV_MAGIC;
    *p = (HCRYPTPROV)x;
    return TRUE;
}

WINADVAPI BOOL WINAPI CryptAcquireContextA(HCRYPTPROV *p, LPCSTR c, LPCSTR prov, DWORD type, DWORD flags)
{
    (void)c; (void)prov;
    return CryptAcquireContextW(p, 0, 0, type, flags);
}

WINADVAPI BOOL WINAPI CryptReleaseContext(HCRYPTPROV p, DWORD flags)
{
    (void)flags;
    Prov *x = (Prov *)p;
    if (!x || x->magic != PROV_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    x->magic = 0;
    LocalFree(x);
    return TRUE;
}

WINADVAPI BOOL WINAPI CryptContextAddRef(HCRYPTPROV p, DWORD *r, DWORD f) { (void)p; (void)r; (void)f; return TRUE; }

WINADVAPI BOOL WINAPI CryptGenRandom(HCRYPTPROV p, DWORD n, BYTE *buf)
{
    (void)p;
    return SystemFunction036(buf, n);
}

static int alg_of(DWORD calg)
{
    switch (calg) {
    case CALG_MD5: return NOVA_MD5;
    case CALG_SHA1: return NOVA_SHA1;
    case CALG_SHA_256: return NOVA_SHA256;
    case CALG_SHA_384: return NOVA_SHA384;
    case CALG_SHA_512: return NOVA_SHA512;
    }
    return -1;
}

WINADVAPI BOOL WINAPI CryptCreateHash(HCRYPTPROV p, DWORD alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *out)
{
    (void)p; (void)flags;
    int a = alg_of(alg);
    if (a < 0 || key) { SetLastError(0x80090008 /* NTE_BAD_ALGID */); return FALSE; }
    Hash *h = LocalAlloc(LMEM_ZEROINIT, sizeof(*h));
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    h->magic = PROV_MAGIC + 1;
    h->alg = alg;
    nova_hash_init(&h->h, a);
    *out = (HCRYPTHASH)h;
    return TRUE;
}

static Hash *hash_of(HCRYPTHASH x)
{
    Hash *h = (Hash *)x;
    if (!h || h->magic != PROV_MAGIC + 1) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return h;
}

WINADVAPI BOOL WINAPI CryptHashData(HCRYPTHASH x, const BYTE *data, DWORD n, DWORD flags)
{
    (void)flags;
    Hash *h = hash_of(x);
    if (!h) return FALSE;
    if (h->done) { SetLastError(0x80090008 /* NTE_BAD_HASH_STATE */); return FALSE; }
    nova_hash_update(&h->h, data, n);
    return TRUE;
}

WINADVAPI BOOL WINAPI CryptGetHashParam(HCRYPTHASH x, DWORD param, BYTE *out, DWORD *n, DWORD flags)
{
    (void)flags;
    Hash *h = hash_of(x);
    if (!h) return FALSE;
    DWORD size = (DWORD)nova_hash_size(h->h.alg);
    if (param == 1 /* HP_ALGID */) {
        if (!out || *n < 4) { *n = 4; SetLastError(ERROR_MORE_DATA); return !out; }
        *(DWORD *)out = h->alg; *n = 4; return TRUE;
    }
    if (param == HP_HASHSIZE) {
        if (!out || *n < 4) { *n = 4; SetLastError(ERROR_MORE_DATA); return !out; }
        *(DWORD *)out = size; *n = 4; return TRUE;
    }
    if (param != HP_HASHVAL) { SetLastError(0x80090009 /* NTE_BAD_TYPE */); return FALSE; }
    if (!out) { *n = size; return TRUE; }
    if (*n < size) { *n = size; SetLastError(ERROR_MORE_DATA); return FALSE; }
    if (!h->done) { nova_hash_final(&h->h, h->digest); h->done = TRUE; }
    for (DWORD i = 0; i < size; i++) out[i] = h->digest[i];
    *n = size;
    return TRUE;
}

WINADVAPI BOOL WINAPI CryptDestroyHash(HCRYPTHASH x)
{
    Hash *h = hash_of(x);
    if (!h) return FALSE;
    h->magic = 0;
    LocalFree(h);
    return TRUE;
}

WINADVAPI BOOL WINAPI CryptDuplicateHash(HCRYPTHASH x, DWORD *r, DWORD f, HCRYPTHASH *out)
{
    (void)r; (void)f;
    Hash *h = hash_of(x);
    if (!h) return FALSE;
    Hash *d = LocalAlloc(0, sizeof(*d));
    if (!d) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *d = *h;
    *out = (HCRYPTHASH)d;
    return TRUE;
}
