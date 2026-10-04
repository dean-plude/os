/*
 * bcrypt.dll — Cryptography API: Next Generation, the parts NovaOS has:
 * the system random number generator, hashes (MD5, SHA-1, SHA-256/384/
 * 512), HMAC and PBKDF2.  Ciphers and public keys are not provided
 * (BCryptOpenAlgorithmProvider reports STATUS_NOT_FOUND for them).
 * BCryptEnumContextFunctions lists Schannel's cipher suites.
 */

#include <winternl.h>
#include "../common/hash.h"

#define BCAPI __declspec(dllexport)

#define STATUS_NOT_FOUND          ((NTSTATUS)0xC0000225)
#define STATUS_BUFFER_TOO_SMALL   ((NTSTATUS)0xC0000023)
#define STATUS_NOT_SUPPORTED      ((NTSTATUS)0xC00000BB)
#define STATUS_INVALID_HANDLE_    ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER_ ((NTSTATUS)0xC000000D)
#define STATUS_NO_MEMORY_         ((NTSTATUS)0xC0000017)

#define HMAC_FLAG 0x00000008            /* BCRYPT_ALG_HANDLE_HMAC_FLAG */
#define RNG_ALG   100

typedef struct { DWORD magic; int alg; BOOL hmac; } Alg;
typedef struct { DWORD magic; int alg; BOOL hmac; NovaHash h; NovaHmac m; } Hash;
#define ALG_MAGIC  0x414C4721u
#define HASH_MAGIC 0x48534821u

static void *zalloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void zfree(void *p) { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }

static int wieq(const WCHAR *a, const char *b)
{
    for (; *a && *b; a++, b++) if ((*a | 0x20) != (*b | 0x20)) return 0;
    return !*a && !*b;
}

/* the pseudo-handles (BCRYPT_*_ALG_HANDLE) */
static BOOL pseudo(ULONG_PTR h, int *alg, BOOL *hmac)
{
    static const int map[] = { -1, -1, NOVA_MD5, NOVA_SHA1, NOVA_SHA256, NOVA_SHA384, NOVA_SHA512, -1, RNG_ALG,
                               NOVA_MD5, NOVA_SHA1, NOVA_SHA256, NOVA_SHA384, NOVA_SHA512 };
    if ((h & 0xF) != 1 || h > 0xD1) return FALSE;
    int i = (int)(h >> 4);
    if (map[i] < 0) return FALSE;
    *alg = map[i];
    *hmac = i >= 9;
    return TRUE;
}

static BOOL alg_of(void *h, int *alg, BOOL *hmac)
{
    if (pseudo((ULONG_PTR)h, alg, hmac)) return TRUE;
    Alg *a = h;
    if (!a || a->magic != ALG_MAGIC) return FALSE;
    *alg = a->alg;
    *hmac = a->hmac;
    return TRUE;
}

BCAPI NTSTATUS WINAPI BCryptOpenAlgorithmProvider(void **out, LPCWSTR id, LPCWSTR impl, ULONG flags)
{
    (void)impl;
    int alg;
    if (wieq(id, "MD5")) alg = NOVA_MD5;
    else if (wieq(id, "SHA1")) alg = NOVA_SHA1;
    else if (wieq(id, "SHA256")) alg = NOVA_SHA256;
    else if (wieq(id, "SHA384")) alg = NOVA_SHA384;
    else if (wieq(id, "SHA512")) alg = NOVA_SHA512;
    else if (wieq(id, "RNG") || wieq(id, "FIPS186DSARNG") || wieq(id, "DUALECRNG")) alg = RNG_ALG;
    else return STATUS_NOT_FOUND;
    Alg *a = zalloc(sizeof(*a));
    if (!a) return STATUS_NO_MEMORY_;
    a->magic = ALG_MAGIC;
    a->alg = alg;
    a->hmac = (flags & HMAC_FLAG) && alg != RNG_ALG;
    *out = a;
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptCloseAlgorithmProvider(void *h, ULONG flags)
{
    (void)flags;
    Alg *a = h;
    int alg; BOOL hm;
    if (pseudo((ULONG_PTR)h, &alg, &hm)) return 0;
    if (!a || a->magic != ALG_MAGIC) return STATUS_INVALID_HANDLE_;
    a->magic = 0;
    zfree(a);
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptGenRandom(void *h, PUCHAR buf, ULONG n, ULONG flags)
{
    int alg; BOOL hm;
    if (!(flags & 2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) && (!alg_of(h, &alg, &hm) || alg != RNG_ALG))
        return STATUS_INVALID_HANDLE_;
    return NtNovaGetRandom(buf, n);
}

static NTSTATUS put_dword(DWORD v, PUCHAR out, ULONG n, ULONG *got)
{
    if (got) *got = 4;
    if (!out) return 0;
    if (n < 4) return STATUS_BUFFER_TOO_SMALL;
    *(DWORD *)out = v;
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptGetProperty(void *h, LPCWSTR prop, PUCHAR out, ULONG n, ULONG *got, ULONG flags)
{
    (void)flags;
    int alg; BOOL hm;
    Hash *x = h;
    if (x && x->magic == HASH_MAGIC) { alg = x->alg; hm = x->hmac; }
    else if (!alg_of(h, &alg, &hm)) return STATUS_INVALID_HANDLE_;
    if (alg == RNG_ALG) return STATUS_NOT_SUPPORTED;
    if (wieq(prop, "ObjectLength")) return put_dword(sizeof(Hash), out, n, got);
    if (wieq(prop, "HashDigestLength")) return put_dword((DWORD)nova_hash_size(alg), out, n, got);
    if (wieq(prop, "HashBlockLength")) return put_dword((DWORD)nova_hash_block(alg), out, n, got);
    if (wieq(prop, "AlgorithmName")) {
        static const char *names[] = { "MD5", "SHA1", "SHA256", "SHA384", "SHA512" };
        const char *s = names[alg];
        ULONG len = 2 * ((ULONG)__builtin_strlen(s) + 1);
        if (got) *got = len;
        if (!out) return 0;
        if (n < len) return STATUS_BUFFER_TOO_SMALL;
        for (ULONG i = 0; i < len / 2; i++) ((WCHAR *)out)[i] = (WCHAR)s[i];
        return 0;
    }
    return STATUS_NOT_SUPPORTED;
}

BCAPI NTSTATUS WINAPI BCryptSetProperty(void *h, LPCWSTR prop, PUCHAR in, ULONG n, ULONG flags)
{
    (void)h; (void)prop; (void)in; (void)n; (void)flags;
    return 0;                               /* e.g. ChainingMode: no ciphers to apply it to */
}

/* The hash object lives on the heap; a caller-supplied buffer is not needed */
BCAPI NTSTATUS WINAPI BCryptCreateHash(void *h, void **out, PUCHAR obj, ULONG objlen, PUCHAR secret, ULONG slen, ULONG flags)
{
    (void)obj; (void)objlen; (void)flags;
    int alg; BOOL hm;
    if (!alg_of(h, &alg, &hm) || alg == RNG_ALG) return STATUS_INVALID_HANDLE_;
    Hash *x = zalloc(sizeof(*x));
    if (!x) return STATUS_NO_MEMORY_;
    x->magic = HASH_MAGIC;
    x->alg = alg;
    x->hmac = hm;
    if (hm) nova_hmac_init(&x->m, alg, secret, slen);
    else nova_hash_init(&x->h, alg);
    *out = x;
    return 0;
}

static Hash *hash_of(void *h)
{
    Hash *x = h;
    return x && x->magic == HASH_MAGIC ? x : 0;
}

BCAPI NTSTATUS WINAPI BCryptHashData(void *h, PUCHAR data, ULONG n, ULONG flags)
{
    (void)flags;
    Hash *x = hash_of(h);
    if (!x) return STATUS_INVALID_HANDLE_;
    if (x->hmac) nova_hmac_update(&x->m, data, n);
    else nova_hash_update(&x->h, data, n);
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptFinishHash(void *h, PUCHAR out, ULONG n, ULONG flags)
{
    (void)flags;
    Hash *x = hash_of(h);
    if (!x) return STATUS_INVALID_HANDLE_;
    if (n != nova_hash_size(x->alg)) return STATUS_INVALID_PARAMETER_;
    if (x->hmac) {
        nova_hmac_final(&x->m, out);
    } else {
        nova_hash_final(&x->h, out);
        nova_hash_init(&x->h, x->alg);      /* reusable, like BCRYPT_HASH_REUSABLE_FLAG */
    }
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptDuplicateHash(void *h, void **out, PUCHAR obj, ULONG objlen, ULONG flags)
{
    (void)obj; (void)objlen; (void)flags;
    Hash *x = hash_of(h);
    if (!x) return STATUS_INVALID_HANDLE_;
    Hash *d = zalloc(sizeof(*d));
    if (!d) return STATUS_NO_MEMORY_;
    *d = *x;
    *out = d;
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptDestroyHash(void *h)
{
    Hash *x = hash_of(h);
    if (!x) return STATUS_INVALID_HANDLE_;
    x->magic = 0;
    zfree(x);
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptHash(void *h, PUCHAR secret, ULONG slen, PUCHAR in, ULONG n, PUCHAR out, ULONG olen)
{
    void *x;
    NTSTATUS s = BCryptCreateHash(h, &x, 0, 0, secret, slen, 0);
    if (s) return s;
    s = BCryptHashData(x, in, n, 0);
    if (!s) s = BCryptFinishHash(x, out, olen, 0);
    BCryptDestroyHash(x);
    return s;
}

/* PBKDF2 (RFC 8018) with the provider's HMAC */
BCAPI NTSTATUS WINAPI BCryptDeriveKeyPBKDF2(void *h, PUCHAR pass, ULONG plen, PUCHAR salt, ULONG slen, ULONGLONG iters,
                                            PUCHAR out, ULONG olen, ULONG flags)
{
    (void)flags;
    int alg; BOOL hm;
    if (!alg_of(h, &alg, &hm) || alg == RNG_ALG || !iters) return STATUS_INVALID_PARAMETER_;
    ULONG hl = (ULONG)nova_hash_size(alg);
    for (ULONG block = 1, done = 0; done < olen; block++) {
        UCHAR u[64], t[64], be[4] = { (UCHAR)(block >> 24), (UCHAR)(block >> 16), (UCHAR)(block >> 8), (UCHAR)block };
        NovaHmac m;
        nova_hmac_init(&m, alg, pass, plen);
        nova_hmac_update(&m, salt, slen);
        nova_hmac_update(&m, be, 4);
        nova_hmac_final(&m, u);
        for (ULONG i = 0; i < hl; i++) t[i] = u[i];
        for (ULONGLONG k = 1; k < iters; k++) {
            nova_hmac_init(&m, alg, pass, plen);
            nova_hmac_update(&m, u, hl);
            nova_hmac_final(&m, u);
            for (ULONG i = 0; i < hl; i++) t[i] ^= u[i];
        }
        for (ULONG i = 0; i < hl && done < olen; i++) out[done++] = t[i];
    }
    return 0;
}

BCAPI NTSTATUS WINAPI BCryptEnumAlgorithms(ULONG ops, ULONG *n, void **list, ULONG flags)
{
    (void)ops; (void)flags;
    *n = 0;
    *list = 0;
    return 0;
}

BCAPI VOID WINAPI BCryptFreeBuffer(PVOID p) { zfree(p); }

/* BCryptEnumContextFunctions: the cipher suites of the local "SSL"
 * context's Schannel interface (NCRYPT_SCHANNEL_INTERFACE), which TLS
 * libraries list to know what Schannel offers.  These are Windows 11's,
 * in its order, all of which secur32's Schannel (Mbed TLS) negotiates.
 * Other tables, contexts and interfaces have none (STATUS_NOT_FOUND). */
typedef struct { ULONG cFunctions; WCHAR **rgpszFunctions; } CtxFunctions;
static const char *const g_ssl_suites[] = {
    "TLS_AES_256_GCM_SHA384", "TLS_AES_128_GCM_SHA256", "TLS_CHACHA20_POLY1305_SHA256",
    "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
    "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
    "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384", "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256",
    "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384", "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256",
    "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA", "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA",
    "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA", "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA",
    "TLS_RSA_WITH_AES_256_GCM_SHA384", "TLS_RSA_WITH_AES_128_GCM_SHA256",
    "TLS_RSA_WITH_AES_256_CBC_SHA256", "TLS_RSA_WITH_AES_128_CBC_SHA256",
    "TLS_RSA_WITH_AES_256_CBC_SHA", "TLS_RSA_WITH_AES_128_CBC_SHA",
};
#define NSUITES (sizeof(g_ssl_suites) / sizeof(g_ssl_suites[0]))

BCAPI NTSTATUS WINAPI BCryptEnumContextFunctions(ULONG table, LPCWSTR ctx, ULONG iface, ULONG *size, void **buf)
{
    if (!ctx || !size || !buf) return STATUS_INVALID_PARAMETER_;
    if (table != 1 /* CRYPT_LOCAL */) return table == 2 /* CRYPT_DOMAIN */ ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER_;
    if (!wieq(ctx, "SSL") || iface != 0x00010002 /* NCRYPT_SCHANNEL_INTERFACE */) return STATUS_NOT_FOUND;
    /* one block: the header, the pointers, then the strings */
    ULONG need = sizeof(CtxFunctions) + NSUITES * sizeof(WCHAR *);
    for (ULONG i = 0; i < NSUITES; i++) need += (ULONG)(__builtin_strlen(g_ssl_suites[i]) + 1) * sizeof(WCHAR);
    BYTE *p = *buf;
    if (p) {
        if (*size < need) { *size = need; return STATUS_BUFFER_TOO_SMALL; }
    } else if (!(p = zalloc(need))) return STATUS_NO_MEMORY_;
    CtxFunctions *f = (CtxFunctions *)p;
    f->cFunctions = NSUITES;
    f->rgpszFunctions = (WCHAR **)(f + 1);
    WCHAR *w = (WCHAR *)(f->rgpszFunctions + NSUITES);
    for (ULONG i = 0; i < NSUITES; i++) {
        f->rgpszFunctions[i] = w;
        for (const char *c = g_ssl_suites[i]; *c; c++) *w++ = (WCHAR)*c;
        *w++ = 0;
    }
    *size = need;
    *buf = p;
    return 0;
}
