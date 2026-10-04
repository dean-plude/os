/*
 * crypt.c — advapi32's random numbers (RtlGenRandom = SystemFunction036,
 * from the kernel's entropy pool) and the CryptoAPI's providers and
 * hashes (MD5, SHA-1, SHA-2), and RtlEncryptMemory.  Keys and ciphers
 * are not provided.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"
#include "../common/hash.h"

WINADVAPI BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG n)
{
    return NT_SUCCESS(NtNovaGetRandom(buf, n));
}

/* RtlEncryptMemory (SystemFunction040) and RtlDecryptMemory
 * (SystemFunction041): @n bytes (a multiple of 8) are XORed with an
 * HMAC-SHA-256 key stream, so the same call undoes them.  The key is the
 * process's own (RTL_ENCRYPT_OPTION_SAME_PROCESS, 0, random on first use)
 * or one every process shares (CROSS_PROCESS 1, SAME_LOGON 2).
 * crypt32's CryptProtectMemory calls these. */
static BYTE g_mem_key[32];
static LONG g_mem_key_made;

static void mem_xor(BYTE *b, ULONG n, ULONG opt)
{
    static const char shared[] = "NovaOS RtlEncryptMemory shared key";
    if (!opt && InterlockedCompareExchange(&g_mem_key_made, 1, 0) == 0) {
        SystemFunction036(g_mem_key, sizeof(g_mem_key));
        g_mem_key_made = 2;
    }
    while (!opt && g_mem_key_made != 2) Sleep(0);
    BYTE ks[32];
    for (ULONG i = 0; i < n; i++) {
        if (!(i & 31)) {
            NovaHmac m;
            ULONG block = i / 32;
            if (opt) nova_hmac_init(&m, NOVA_SHA256, shared, sizeof(shared) - 1);
            else nova_hmac_init(&m, NOVA_SHA256, g_mem_key, sizeof(g_mem_key));
            nova_hmac_update(&m, &block, sizeof(block));
            nova_hmac_final(&m, ks);
        }
        b[i] ^= ks[i & 31];
    }
}

WINADVAPI NTSTATUS WINAPI SystemFunction040(PVOID mem, ULONG n, ULONG opt)
{
    if (!mem || (n & 7) || opt > 2) return 0xC000000DL;       /* STATUS_INVALID_PARAMETER */
    mem_xor(mem, n, opt);
    return 0;
}

WINADVAPI NTSTATUS WINAPI SystemFunction041(PVOID mem, ULONG n, ULONG opt) { return SystemFunction040(mem, n, opt); }

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

/* -----------------------------------------------------------------------
 * Keys: the providers here hash and give random bytes; they hold no keys,
 * so key and signature operations fail as for a container without one
 * ----------------------------------------------------------------------- */
#define NTE_NO_KEY_      0x8009000DL
#define NTE_BAD_KEY_     0x80090003L
#define NTE_BAD_TYPE_    0x8009000AL
#define NTE_NO_MORE_ITEMS_ 0x80090018L    /* ERROR_NO_MORE_ITEMS for enumerations: 259 */
WINADVAPI BOOL WINAPI CryptGetUserKey(HCRYPTPROV p, DWORD spec, HCRYPTKEY *k) { (void)p; (void)spec; if (k) *k = 0; SetLastError(NTE_NO_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptGenKey(HCRYPTPROV p, DWORD alg, DWORD flags, HCRYPTKEY *k) { (void)p; (void)alg; (void)flags; if (k) *k = 0; SetLastError(NTE_BAD_TYPE_); return FALSE; }
WINADVAPI BOOL WINAPI CryptImportKey(HCRYPTPROV p, const BYTE *d, DWORD n, HCRYPTKEY pub, DWORD flags, HCRYPTKEY *k)
{ (void)p; (void)d; (void)n; (void)pub; (void)flags; if (k) *k = 0; SetLastError(NTE_BAD_TYPE_); return FALSE; }
WINADVAPI BOOL WINAPI CryptExportKey(HCRYPTKEY k, HCRYPTKEY e, DWORD t, DWORD f, BYTE *d, DWORD *n) { (void)k; (void)e; (void)t; (void)f; (void)d; (void)n; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptDestroyKey(HCRYPTKEY k) { (void)k; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptEncrypt(HCRYPTKEY k, HCRYPTHASH h, BOOL fin, DWORD f, BYTE *d, DWORD *n, DWORD len)
{ (void)k; (void)h; (void)fin; (void)f; (void)d; (void)n; (void)len; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptDecrypt(HCRYPTKEY k, HCRYPTHASH h, BOOL fin, DWORD f, BYTE *d, DWORD *n)
{ (void)k; (void)h; (void)fin; (void)f; (void)d; (void)n; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptGetKeyParam(HCRYPTKEY k, DWORD p, BYTE *d, DWORD *n, DWORD f) { (void)k; (void)p; (void)d; (void)n; (void)f; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptSetKeyParam(HCRYPTKEY k, DWORD p, const BYTE *d, DWORD f) { (void)k; (void)p; (void)d; (void)f; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptSetHashParam(HCRYPTHASH h, DWORD p, const BYTE *d, DWORD f) { (void)h; (void)p; (void)d; (void)f; SetLastError(NTE_BAD_TYPE_); return FALSE; }
WINADVAPI BOOL WINAPI CryptSignHashW(HCRYPTHASH h, DWORD spec, LPCWSTR desc, DWORD f, BYTE *sig, DWORD *n)
{ (void)h; (void)spec; (void)desc; (void)f; (void)sig; (void)n; SetLastError(NTE_NO_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptSignHashA(HCRYPTHASH h, DWORD spec, LPCSTR desc, DWORD f, BYTE *sig, DWORD *n)
{ (void)h; (void)spec; (void)desc; (void)f; (void)sig; (void)n; SetLastError(NTE_NO_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptVerifySignatureA(HCRYPTHASH h, const BYTE *sig, DWORD n, HCRYPTKEY k, LPCSTR desc, DWORD f)
{ (void)h; (void)sig; (void)n; (void)k; (void)desc; (void)f; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptVerifySignatureW(HCRYPTHASH h, const BYTE *sig, DWORD n, HCRYPTKEY k, LPCWSTR desc, DWORD f)
{ (void)h; (void)sig; (void)n; (void)k; (void)desc; (void)f; SetLastError(NTE_BAD_KEY_); return FALSE; }
WINADVAPI BOOL WINAPI CryptGetProvParam(HCRYPTPROV p, DWORD param, BYTE *d, DWORD *n, DWORD f)
{
    (void)p; (void)f;
    if (param == 2 /* PP_ENUMCONTAINERS */) { SetLastError(259 /* ERROR_NO_MORE_ITEMS */); return FALSE; }
    if (param == 4 /* PP_NAME */) {
        static const char name[] = "Microsoft Enhanced RSA and AES Cryptographic Provider";
        if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (!d) { *n = sizeof(name); return TRUE; }
        if (*n < sizeof(name)) { *n = sizeof(name); SetLastError(ERROR_MORE_DATA); return FALSE; }
        CopyMemory(d, name, sizeof(name));
        *n = sizeof(name);
        return TRUE;
    }
    SetLastError(NTE_BAD_TYPE_);
    return FALSE;
}
/* One provider: the enhanced RSA/AES one (type PROV_RSA_AES, 24) */
WINADVAPI BOOL WINAPI CryptEnumProvidersW(DWORD i, DWORD *r, DWORD f, DWORD *type, LPWSTR name, DWORD *n)
{
    (void)r; (void)f;
    static const WCHAR pn[] = L"Microsoft Enhanced RSA and AES Cryptographic Provider";
    if (i) { SetLastError(259); return FALSE; }
    if (type) *type = 24;
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!name) { *n = sizeof(pn); return TRUE; }
    if (*n < sizeof(pn)) { *n = sizeof(pn); SetLastError(ERROR_MORE_DATA); return FALSE; }
    CopyMemory(name, pn, sizeof(pn));
    *n = sizeof(pn);
    return TRUE;
}
