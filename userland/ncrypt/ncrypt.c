/*
 * ncrypt.dll — CNG key storage.  NovaOS has no key storage provider, so a
 * provider opens (programs probe it) but no key can be created, imported or
 * opened in it.
 */
#include <windows.h>

#define NC __declspec(dllexport)
typedef LONG SECURITY_STATUS;
typedef ULONG_PTR NCRYPT_HANDLE;
#define NTE_BAD_KEYSET_     ((SECURITY_STATUS)0x80090016L)
#define NTE_INVALID_HANDLE_ ((SECURITY_STATUS)0x80090026L)
#define NTE_INVALID_PARAMETER_ ((SECURITY_STATUS)0x80090027L)
#define NTE_NOT_SUPPORTED_  ((SECURITY_STATUS)0x80090029L)
#define NTE_NO_MORE_ITEMS_  ((SECURITY_STATUS)0x8009002AL)

#define PROVIDER_MAGIC 0x4E435250                       /* "NCRP" */
typedef struct { DWORD magic; } Provider;

NC SECURITY_STATUS WINAPI NCryptOpenStorageProvider(NCRYPT_HANDLE *prov, LPCWSTR name, DWORD flags)
{
    (void)name; (void)flags;
    if (!prov) return NTE_INVALID_PARAMETER_;
    Provider *p = HeapAlloc(GetProcessHeap(), 0, sizeof(*p));
    if (!p) return (SECURITY_STATUS)0x8009000EL;        /* NTE_NO_MEMORY */
    p->magic = PROVIDER_MAGIC;
    *prov = (NCRYPT_HANDLE)p;
    return 0;
}
static int is_provider(NCRYPT_HANDLE h) { return h && ((Provider *)h)->magic == PROVIDER_MAGIC; }

NC SECURITY_STATUS WINAPI NCryptFreeObject(NCRYPT_HANDLE h)
{
    if (!is_provider(h)) return NTE_INVALID_HANDLE_;
    ((Provider *)h)->magic = 0;
    HeapFree(GetProcessHeap(), 0, (void *)h);
    return 0;
}
NC SECURITY_STATUS WINAPI NCryptFreeBuffer(PVOID p) { if (p) HeapFree(GetProcessHeap(), 0, p); return 0; }
NC BOOL WINAPI NCryptIsKeyHandle(NCRYPT_HANDLE h) { (void)h; return FALSE; }

NC SECURITY_STATUS WINAPI NCryptOpenKey(NCRYPT_HANDLE prov, NCRYPT_HANDLE *key, LPCWSTR name, DWORD spec, DWORD flags)
{
    (void)name; (void)spec; (void)flags;
    if (key) *key = 0;
    return is_provider(prov) ? NTE_BAD_KEYSET_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptCreatePersistedKey(NCRYPT_HANDLE prov, NCRYPT_HANDLE *key, LPCWSTR alg, LPCWSTR name,
                                                   DWORD spec, DWORD flags)
{
    (void)alg; (void)name; (void)spec; (void)flags;
    if (key) *key = 0;
    return is_provider(prov) ? NTE_NOT_SUPPORTED_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptImportKey(NCRYPT_HANDLE prov, NCRYPT_HANDLE imp, LPCWSTR type, PVOID params,
                                          NCRYPT_HANDLE *key, PBYTE data, DWORD n, DWORD flags)
{
    (void)imp; (void)type; (void)params; (void)data; (void)n; (void)flags;
    if (key) *key = 0;
    return is_provider(prov) ? NTE_NOT_SUPPORTED_ : NTE_INVALID_HANDLE_;
}
/* No key handle can exist, so every per-key call has a bad handle */
NC SECURITY_STATUS WINAPI NCryptFinalizeKey(NCRYPT_HANDLE key, DWORD flags) { (void)key; (void)flags; return NTE_INVALID_HANDLE_; }
NC SECURITY_STATUS WINAPI NCryptDeleteKey(NCRYPT_HANDLE key, DWORD flags) { (void)key; (void)flags; return NTE_INVALID_HANDLE_; }
NC SECURITY_STATUS WINAPI NCryptExportKey(NCRYPT_HANDLE key, NCRYPT_HANDLE exp, LPCWSTR type, PVOID params, PBYTE out,
                                          DWORD cap, DWORD *n, DWORD flags)
{
    (void)key; (void)exp; (void)type; (void)params; (void)out; (void)cap; (void)flags;
    if (n) *n = 0;
    return NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptSetProperty(NCRYPT_HANDLE h, LPCWSTR prop, PBYTE val, DWORD n, DWORD flags)
{
    (void)prop; (void)val; (void)n; (void)flags;
    return is_provider(h) ? NTE_NOT_SUPPORTED_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptGetProperty(NCRYPT_HANDLE h, LPCWSTR prop, PBYTE out, DWORD cap, DWORD *n, DWORD flags)
{
    (void)prop; (void)out; (void)cap; (void)flags;
    if (n) *n = 0;
    return is_provider(h) ? NTE_NOT_SUPPORTED_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptEnumKeys(NCRYPT_HANDLE prov, LPCWSTR scope, PVOID *item, PVOID *state, DWORD flags)
{
    (void)scope; (void)state; (void)flags;
    if (item) *item = 0;
    return is_provider(prov) ? NTE_NO_MORE_ITEMS_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptEnumAlgorithms(NCRYPT_HANDLE prov, DWORD ops, DWORD *n, PVOID *list, DWORD flags)
{
    (void)ops; (void)flags;
    if (n) *n = 0;
    if (list) *list = 0;
    return is_provider(prov) ? 0 : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptIsAlgSupported(NCRYPT_HANDLE prov, LPCWSTR alg, DWORD flags)
{
    (void)alg; (void)flags;
    return is_provider(prov) ? NTE_NOT_SUPPORTED_ : NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptSignHash(NCRYPT_HANDLE key, PVOID pad, PBYTE hash, DWORD hn, PBYTE sig, DWORD cap,
                                         DWORD *n, DWORD flags)
{
    (void)key; (void)pad; (void)hash; (void)hn; (void)sig; (void)cap; (void)flags;
    if (n) *n = 0;
    return NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptVerifySignature(NCRYPT_HANDLE key, PVOID pad, PBYTE hash, DWORD hn, PBYTE sig, DWORD sn,
                                                DWORD flags)
{
    (void)key; (void)pad; (void)hash; (void)hn; (void)sig; (void)sn; (void)flags;
    return NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptEncrypt(NCRYPT_HANDLE key, PBYTE in, DWORD inn, PVOID pad, PBYTE out, DWORD cap,
                                        DWORD *n, DWORD flags)
{
    (void)key; (void)in; (void)inn; (void)pad; (void)out; (void)cap; (void)flags;
    if (n) *n = 0;
    return NTE_INVALID_HANDLE_;
}
NC SECURITY_STATUS WINAPI NCryptDecrypt(NCRYPT_HANDLE key, PBYTE in, DWORD inn, PVOID pad, PBYTE out, DWORD cap,
                                        DWORD *n, DWORD flags)
{
    (void)key; (void)in; (void)inn; (void)pad; (void)out; (void)cap; (void)flags;
    if (n) *n = 0;
    return NTE_INVALID_HANDLE_;
}
