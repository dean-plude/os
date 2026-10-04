/*
 * protect.c — the Data Protection API (CryptProtectData and
 * CryptUnprotectData).
 *
 * Windows seals data with a key only the user (or, with
 * CRYPTPROTECT_LOCAL_MACHINE, anyone on the machine) can use, and the blob
 * is opaque to the program.  NovaOS keeps one random 256-bit master key per
 * user (HKCU\Software\Microsoft\Protect) and one for the machine
 * (HKLM\SOFTWARE\Microsoft\Cryptography\Protect), made the first time
 * they are needed.  Each blob gets a random salt; the encryption and MAC
 * keys are HMAC-SHA256 of the master key over the salt and the caller's
 * optional entropy, the data is XORed with HMAC-SHA256 in counter mode and
 * the whole blob is authenticated with HMAC-SHA256 (encrypt-then-MAC), so
 * a changed blob or the wrong entropy fails as on Windows (NTE_BAD_DATA).
 *
 * Blob: "NDPA", flags, salt[16], description length (bytes, with its NUL),
 * description (UTF-16), data length, data, tag[32].
 */
#include <windows.h>
#include <winternl.h>
#include <string.h>
#include "../common/hash.h"

#define CRYPT32API __declspec(dllexport)
#define NTE_BAD_DATA_         ((DWORD)0x80090005L)
#define CRYPTPROTECT_LOCAL_MACHINE 0x4
#define CRYPTPROTECT_UI_FORBIDDEN  0x1

typedef struct { DWORD cbData; BYTE *pbData; } Blob;

static const char MAGIC[4] = { 'N', 'D', 'P', 'A' };

/* the master key of the user or the machine, made on first use */
static BOOL master_key(BOOL machine, BYTE key[32])
{
    static const WCHAR user_path[] = L"Software\\Microsoft\\Protect";
    static const WCHAR machine_path[] = L"SOFTWARE\\Microsoft\\Cryptography\\Protect";
    HKEY k;
    if (RegCreateKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, machine ? machine_path : user_path,
                        0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0))
        return FALSE;
    DWORD type = 0, n = 32;
    LSTATUS e = RegQueryValueExW(k, L"MasterKey", 0, &type, key, &n);
    if (e || type != REG_BINARY || n != 32) {
        e = NtNovaGetRandom(key, 32) ? 1 : RegSetValueExW(k, L"MasterKey", 0, REG_BINARY, key, 32);
    }
    RegCloseKey(k);
    return !e;
}

/* HMAC-SHA256(@key, @label || @salt || SHA-256(@entropy)) */
static void derive(const BYTE master[32], char label, const BYTE salt[16], const Blob *entropy, BYTE out[32])
{
    BYTE eh[32];
    NovaHash h;
    nova_hash_init(&h, NOVA_SHA256);
    if (entropy && entropy->pbData) nova_hash_update(&h, entropy->pbData, entropy->cbData);
    nova_hash_final(&h, eh);
    NovaHmac m;
    nova_hmac_init(&m, NOVA_SHA256, master, 32);
    nova_hmac_update(&m, &label, 1);
    nova_hmac_update(&m, salt, 16);
    nova_hmac_update(&m, eh, 32);
    nova_hmac_final(&m, out);
}

/* XOR @n bytes at @p with HMAC-SHA256(@key, counter) blocks */
static void crypt_stream(const BYTE key[32], BYTE *p, DWORD n)
{
    BYTE block[32];
    for (DWORD i = 0, ctr = 0; i < n; ctr++) {
        NovaHmac m;
        nova_hmac_init(&m, NOVA_SHA256, key, 32);
        nova_hmac_update(&m, &ctr, 4);
        nova_hmac_final(&m, block);
        for (int j = 0; j < 32 && i < n; j++, i++) p[i] ^= block[j];
    }
}

static void tag_of(const BYTE key[32], const BYTE *p, DWORD n, BYTE tag[32])
{
    NovaHmac m;
    nova_hmac_init(&m, NOVA_SHA256, key, 32);
    nova_hmac_update(&m, p, n);
    nova_hmac_final(&m, tag);
}

static DWORD wbytes(LPCWSTR s) { DWORD n = 0; if (s) while (s[n]) n++; return s ? (n + 1) * 2 : 0; }

CRYPT32API BOOL WINAPI CryptProtectData(Blob *in, LPCWSTR desc, Blob *entropy, PVOID reserved, PVOID prompt,
                                        DWORD flags, Blob *out)
{
    (void)reserved; (void)prompt;
    if (!in || !out || (!in->pbData && in->cbData)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    out->cbData = 0;
    out->pbData = 0;
    BOOL machine = (flags & CRYPTPROTECT_LOCAL_MACHINE) != 0;
    BYTE master[32], salt[16], ek[32], mk[32];
    if (!master_key(machine, master) || NtNovaGetRandom(salt, 16)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    DWORD dn = wbytes(desc), total = 4 + 4 + 16 + 4 + dn + 4 + in->cbData + 32;
    BYTE *b = LocalAlloc(LMEM_FIXED, total), *p = b;
    if (!b) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    DWORD f = machine;
    memcpy(p, MAGIC, 4); p += 4;
    memcpy(p, &f, 4); p += 4;
    memcpy(p, salt, 16); p += 16;
    memcpy(p, &dn, 4); p += 4;
    if (dn) memcpy(p, desc, dn);
    p += dn;
    memcpy(p, &in->cbData, 4); p += 4;
    if (in->cbData) memcpy(p, in->pbData, in->cbData);
    derive(master, 'e', salt, entropy, ek);
    derive(master, 'm', salt, entropy, mk);
    crypt_stream(ek, p, in->cbData);
    p += in->cbData;
    tag_of(mk, b, (DWORD)(p - b), p);
    SecureZeroMemory(master, sizeof master);
    SecureZeroMemory(ek, sizeof ek);
    SecureZeroMemory(mk, sizeof mk);
    out->cbData = total;
    out->pbData = b;
    return TRUE;
}

CRYPT32API BOOL WINAPI CryptUnprotectData(Blob *in, LPWSTR *desc, Blob *entropy, PVOID reserved, PVOID prompt,
                                          DWORD flags, Blob *out)
{
    (void)reserved; (void)prompt; (void)flags;
    if (desc) *desc = 0;
    if (!in || !out || !in->pbData) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    out->cbData = 0;
    out->pbData = 0;
    const BYTE *b = in->pbData, *p = b;
    DWORD n = in->cbData, f, dn, cn;
    if (n < 4 + 4 + 16 + 4 + 4 + 32 || memcmp(b, MAGIC, 4)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    memcpy(&f, b + 4, 4);
    memcpy(&dn, b + 24, 4);
    if (dn > n - 60 || (dn & 1)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    memcpy(&cn, b + 28 + dn, 4);
    if (cn != n - 64 - dn) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    BYTE master[32], ek[32], mk[32], tag[32];
    if (!master_key((f & 1) != 0, master)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    derive(master, 'e', b + 8, entropy, ek);
    derive(master, 'm', b + 8, entropy, mk);
    SecureZeroMemory(master, sizeof master);
    tag_of(mk, b, n - 32, tag);
    BYTE diff = 0;
    for (int i = 0; i < 32; i++) diff |= (BYTE)(tag[i] ^ b[n - 32 + i]);
    if (diff) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    p = b + 32 + dn;
    BYTE *data = LocalAlloc(LMEM_FIXED, cn ? cn : 1);
    LPWSTR d = desc ? LocalAlloc(LMEM_FIXED, dn ? dn : 2) : 0;
    if (!data || (desc && !d)) {
        LocalFree(data);
        LocalFree(d);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memcpy(data, p, cn);
    crypt_stream(ek, data, cn);
    SecureZeroMemory(ek, sizeof ek);
    if (d) {
        if (dn) memcpy(d, b + 28, dn); else d[0] = 0;
        *desc = d;
    }
    out->cbData = cn;
    out->pbData = data;
    return TRUE;
}
