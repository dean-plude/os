/*
 * crypt32.dll — encoding helpers, names, messages.  Certificates, stores
 * and chains are in certs.c.
 */
#include <windows.h>

#define CRYPT32API __declspec(dllexport)
#define CRYPT_E_NOT_FOUND_ 0x80092004L

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

/* Revocation lists: none in the stores */
CRYPT32API const void *WINAPI CertEnumCRLsInStore(HANDLE h, const void *prev) { (void)h; (void)prev; SetLastError(CRYPT_E_NOT_FOUND_); return 0; }
CRYPT32API BOOL WINAPI CertFreeCRLContext(const void *c) { (void)c; return TRUE; }

/* Base64 and hex text back to bytes (CRYPT_STRING_BASE64HEADER, BASE64,
 * BINARY, HEX, HEXRAW and the ANY forms) */
static int b64v(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' ? 62 : c == '/' ? 63 : -1;
}
static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
}
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Decode [s, s + n) of characters fetched by @at; returns the byte count
 * or -1 */
static long decode_text(const void *s, DWORD n, int wide, DWORD fmt, BYTE *out, DWORD cap)
{
#define CH(i) (wide ? ((const WCHAR *)s)[i] : (BYTE)((const char *)s)[i])
    DWORD i = 0, end = n;
    if (fmt == 0 || fmt == 6 || fmt == 7) {             /* skip a "-----BEGIN ...-----" line */
        for (DWORD k = 0; k + 5 <= n; k++)
            if (CH(k) == '-' && CH(k + 1) == '-' && CH(k + 2) == '-' && CH(k + 3) == '-' && CH(k + 4) == '-') {
                DWORD e = k + 5;
                while (e < n && CH(e) != '\n') e++;
                if (e < n) { i = e + 1; for (DWORD m = i; m + 5 <= n; m++) if (CH(m) == '-' && CH(m + 1) == '-') { end = m; break; } }
                break;
            }
        if (fmt == 0 && i == 0) return -1;
    }
    long o = 0;
    if (fmt == 2) {                                    /* CRYPT_STRING_BINARY: as is */
        for (; i < end; i++) { if (out && (DWORD)o < cap) out[o] = (BYTE)CH(i); o++; }
        return o;
    }
    if (fmt == 4 || fmt == 12) {                       /* HEX / HEXRAW */
        int hi = -1;
        for (; i < end; i++) {
            int c = CH(i);
            if (is_ws(c)) continue;
            int v = hexv(c);
            if (v < 0) return -1;
            if (hi < 0) hi = v;
            else { if (out && (DWORD)o < cap) out[o] = (BYTE)(hi << 4 | v); o++; hi = -1; }
        }
        return hi < 0 ? o : -1;
    }
    DWORD acc = 0;
    int bits = 0;
    for (; i < end; i++) {
        int c = CH(i);
        if (is_ws(c)) continue;
        if (c == '=') break;
        int v = b64v(c);
        if (v < 0) return -1;
        acc = acc << 6 | (DWORD)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (out && (DWORD)o < cap) out[o] = (BYTE)(acc >> bits); o++; }
    }
    return o;
#undef CH
}

static BOOL string_to_binary(const void *s, DWORD n, int wide, DWORD flags, BYTE *out, DWORD *len, DWORD *skip, DWORD *used)
{
    if (!s || !len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!n) n = wide ? (DWORD)lstrlenW(s) : (DWORD)lstrlenA(s);
    DWORD fmt = flags & 0xFF;
    static const DWORD any[] = { 0, 1, 2 }, b64any[] = { 0, 1 };
    const DWORD *try = &fmt;
    int ntry = 1;
    if (fmt == 7) { try = any; ntry = 3; }               /* CRYPT_STRING_ANY */
    else if (fmt == 6) { try = b64any; ntry = 2; }       /* CRYPT_STRING_BASE64_ANY */
    else if (fmt == 8) fmt = 4;                          /* HEX_ANY */
    for (int t = 0; t < ntry; t++) {
        long r = decode_text(s, n, wide, try[t], 0, 0);
        if (r < 0) continue;
        if (out && *len < (DWORD)r) { *len = (DWORD)r; SetLastError(ERROR_MORE_DATA); return FALSE; }
        if (out) decode_text(s, n, wide, try[t], out, *len);
        *len = (DWORD)r;
        if (skip) *skip = 0;
        if (used) *used = try[t];
        return TRUE;
    }
    SetLastError(0x8009310B);                            /* CRYPT_E_ASN1_BADTAG */
    return FALSE;
}

CRYPT32API BOOL WINAPI CryptStringToBinaryA(LPCSTR s, DWORD n, DWORD flags, BYTE *out, DWORD *len, DWORD *skip, DWORD *used)
{ return string_to_binary(s, n, 0, flags, out, len, skip, used); }
CRYPT32API BOOL WINAPI CryptStringToBinaryW(LPCWSTR s, DWORD n, DWORD flags, BYTE *out, DWORD *len, DWORD *skip, DWORD *used)
{ return string_to_binary(s, n, 1, flags, out, len, skip, used); }

/* Certificates are not parsed: there is nothing to read names or
 * extensions from, decode, sign or build chains with */
#define CRYPT_E_NO_MATCH_     0x80092009L
#define CRYPT_E_ASN1_BADTAG_  0x8009310BL
#define NTE_NOT_SUPPORTED_    0x80090029L

static DWORD empty_name_w(LPWSTR s, DWORD n) { if (s && n) s[0] = 0; return 1; }
static DWORD empty_name_a(LPSTR s, DWORD n) { if (s && n) s[0] = 0; return 1; }
CRYPT32API DWORD WINAPI CertNameToStrW(DWORD enc, const void *name, DWORD type, LPWSTR s, DWORD n)
{ (void)enc; (void)name; (void)type; return empty_name_w(s, n); }
CRYPT32API DWORD WINAPI CertNameToStrA(DWORD enc, const void *name, DWORD type, LPSTR s, DWORD n)
{ (void)enc; (void)name; (void)type; return empty_name_a(s, n); }
CRYPT32API DWORD WINAPI CertGetNameStringW(const void *c, DWORD type, DWORD flags, void *para, LPWSTR s, DWORD n)
{ (void)c; (void)type; (void)flags; (void)para; return empty_name_w(s, n); }
CRYPT32API DWORD WINAPI CertGetNameStringA(const void *c, DWORD type, DWORD flags, void *para, LPSTR s, DWORD n)
{ (void)c; (void)type; (void)flags; (void)para; return empty_name_a(s, n); }
CRYPT32API BOOL WINAPI CertStrToNameA(DWORD enc, LPCSTR x, DWORD type, void *r, BYTE *out, DWORD *n, LPCSTR *err)
{ (void)enc; (void)x; (void)type; (void)r; (void)out; (void)n; if (err) *err = x; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API BOOL WINAPI CertStrToNameW(DWORD enc, LPCWSTR x, DWORD type, void *r, BYTE *out, DWORD *n, LPCWSTR *err)
{ (void)enc; (void)x; (void)type; (void)r; (void)out; (void)n; if (err) *err = x; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API BOOL WINAPI CryptQueryObject(DWORD type, const void *obj, DWORD ct, DWORD ft, DWORD flags, DWORD *enc,
                                        DWORD *ctype, DWORD *ftype, HANDLE *store, HANDLE *msg, const void **ctx)
{
    (void)type; (void)obj; (void)ct; (void)ft; (void)flags; (void)enc; (void)ctype; (void)ftype;
    if (store) *store = 0;
    if (msg) *msg = 0;
    if (ctx) *ctx = 0;
    SetLastError(CRYPT_E_NO_MATCH_);
    return FALSE;
}
CRYPT32API BOOL WINAPI CryptMsgGetParam(HANDLE msg, DWORD type, DWORD index, void *data, DWORD *n)
{ (void)msg; (void)type; (void)index; (void)data; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
CRYPT32API BOOL WINAPI CryptMsgClose(HANDLE msg) { (void)msg; return TRUE; }
CRYPT32API BOOL WINAPI CryptDecodeObjectEx(DWORD enc, LPCSTR type, const BYTE *b, DWORD n, DWORD flags, void *para, void *out, DWORD *len)
{ (void)enc; (void)type; (void)b; (void)n; (void)flags; (void)para; (void)out; (void)len; SetLastError(CRYPT_E_ASN1_BADTAG_); return FALSE; }
CRYPT32API BOOL WINAPI CryptDecodeObject(DWORD enc, LPCSTR type, const BYTE *b, DWORD n, DWORD flags, void *out, DWORD *len)
{ (void)enc; (void)type; (void)b; (void)n; (void)flags; (void)out; (void)len; SetLastError(CRYPT_E_ASN1_BADTAG_); return FALSE; }
CRYPT32API BOOL WINAPI CryptEncodeObjectEx(DWORD enc, LPCSTR type, const void *v, DWORD flags, void *para, void *out, DWORD *len)
{ (void)enc; (void)type; (void)v; (void)flags; (void)para; (void)out; (void)len; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API BOOL WINAPI CryptEncodeObject(DWORD enc, LPCSTR type, const void *v, BYTE *out, DWORD *len)
{ (void)enc; (void)type; (void)v; (void)out; (void)len; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API const void *WINAPI CertCreateSelfSignCertificate(HANDLE key, void *subject, DWORD flags, void *kpi, void *alg,
                                                             void *start, void *end, void *ext)
{ (void)key; (void)subject; (void)flags; (void)kpi; (void)alg; (void)start; (void)end; (void)ext; SetLastError(NTE_NOT_SUPPORTED_); return 0; }
CRYPT32API BOOL WINAPI CertSetCertificateContextProperty(const void *c, DWORD id, DWORD flags, const void *data)
{ (void)c; (void)id; (void)flags; (void)data; return TRUE; }
CRYPT32API BOOL WINAPI CertGetIntendedKeyUsage(DWORD enc, void *info, BYTE *usage, DWORD n)
{ (void)enc; (void)info; if (usage && n) ZeroMemory(usage, n); SetLastError(0); return FALSE; }
CRYPT32API void *WINAPI CertFindExtension(LPCSTR oid, DWORD n, void *ext) { (void)oid; (void)n; (void)ext; return 0; }
CRYPT32API BOOL WINAPI CryptHashCertificate2(LPCWSTR alg, DWORD flags, void *r, const BYTE *b, DWORD n, BYTE *hash, DWORD *len)
{ (void)alg; (void)flags; (void)r; (void)b; (void)n; (void)hash; (void)len; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API HANDLE WINAPI PFXImportCertStore(void *pfx, LPCWSTR pw, DWORD flags)
{ (void)pfx; (void)pw; (void)flags; SetLastError(CRYPT_E_ASN1_BADTAG_); return 0; }
CRYPT32API BOOL WINAPI CertCreateCertificateChainEngine(void *config, HANDLE *engine)
{ (void)config; if (engine) *engine = 0; SetLastError(NTE_NOT_SUPPORTED_); return FALSE; }
CRYPT32API VOID WINAPI CertFreeCertificateChainEngine(HANDLE engine) { (void)engine; }

CRYPT32API BOOL WINAPI CryptBinaryToStringW(const BYTE *b, DWORD n, DWORD flags, LPWSTR out, DWORD *len)
{
    DWORD need = 0;
    if (!CryptBinaryToStringA(b, n, flags, 0, &need)) return FALSE;
    if (!out) { *len = need; return TRUE; }
    if (*len < need) { *len = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    char *a = HeapAlloc(GetProcessHeap(), 0, need);
    if (!a) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    DWORD k = need;
    BOOL ok = CryptBinaryToStringA(b, n, flags, a, &k);
    if (ok) { for (DWORD i = 0; i <= k; i++) out[i] = (WCHAR)(BYTE)a[i]; *len = k; }
    HeapFree(GetProcessHeap(), 0, a);
    return ok;
}

/* Signed messages (PKCS #7) and certificate chains to a private key:
 * no message decoder and no keys in the stores */
CRYPT32API HANDLE WINAPI CryptMsgOpenToDecode(DWORD enc, DWORD flags, DWORD type, HANDLE prov, void *recip, const void *stream)
{
    (void)enc; (void)flags; (void)type; (void)prov; (void)recip; (void)stream;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
CRYPT32API BOOL WINAPI CryptMsgUpdate(HANDLE msg, const BYTE *data, DWORD n, BOOL final)
{
    (void)msg; (void)data; (void)n; (void)final;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
CRYPT32API const void *WINAPI CertFindChainInStore(HANDLE store, DWORD enc, DWORD flags, DWORD type, const void *para, const void *prev)
{
    (void)store; (void)enc; (void)flags; (void)type; (void)para; (void)prev;
    SetLastError(CRYPT_E_NOT_FOUND_);
    return 0;
}
CRYPT32API BOOL WINAPI CryptAcquireCertificatePrivateKey(const void *cert, DWORD flags, void *para, HANDLE *key, DWORD *spec, BOOL *free_key)
{
    (void)cert; (void)flags; (void)para; (void)spec;
    if (key) *key = 0;
    if (free_key) *free_key = FALSE;
    SetLastError(0x8009200B);                       /* CRYPT_E_NO_KEY_PROPERTY */
    return FALSE;
}

/* CryptProtectMemory and CryptUnprotectMemory: advapi32's RtlEncryptMemory
 * (SystemFunction040/041) on blocks of 16 bytes
 * (CRYPTPROTECTMEMORY_BLOCK_SIZE); the flags are the same */
static BOOL protect_memory(void *mem, DWORD n, DWORD flags, const char *fn)
{
    typedef LONG(WINAPI * Rtl)(void *, ULONG, ULONG);
    if (!mem || (n & 15) || flags > 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Rtl f = (Rtl)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), fn);
    if (!f || f(mem, n, flags) < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
CRYPT32API BOOL WINAPI CryptProtectMemory(void *mem, DWORD n, DWORD flags) { return protect_memory(mem, n, flags, "SystemFunction040"); }
CRYPT32API BOOL WINAPI CryptUnprotectMemory(void *mem, DWORD n, DWORD flags) { return protect_memory(mem, n, flags, "SystemFunction041"); }
