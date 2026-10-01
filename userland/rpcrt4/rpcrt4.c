/*
 * rpcrt4.dll — the RPC runtime's UUID helpers: new UUIDs (random, version
 * 4; the "sequential" ones too), and their string forms.
 */
#include <windows.h>
#include <winternl.h>

#define RPCAPI __declspec(dllexport)
#define RPC_S_OK_ 0
#define RPC_S_INVALID_STRING_UUID_ 1705
#define RPC_S_UUID_LOCAL_ONLY_ 1824

RPCAPI LONG WINAPI UuidCreate(GUID *u)
{
    if (!u) return ERROR_INVALID_PARAMETER;
    NtNovaGetRandom(u, sizeof(*u));
    u->Data3 = (WORD)((u->Data3 & 0x0FFF) | 0x4000);             /* version 4 */
    u->Data4[0] = (BYTE)((u->Data4[0] & 0x3F) | 0x80);          /* RFC 4122 variant */
    return RPC_S_OK_;
}
RPCAPI LONG WINAPI UuidCreateSequential(GUID *u) { return UuidCreate(u); }
RPCAPI LONG WINAPI UuidCreateNil(GUID *u) { if (u) ZeroMemory(u, sizeof(*u)); return RPC_S_OK_; }
RPCAPI int WINAPI UuidIsNil(GUID *u, LONG *st)
{
    if (st) *st = RPC_S_OK_;
    if (!u) return 1;
    const BYTE *b = (const BYTE *)u;
    for (int i = 0; i < 16; i++) if (b[i]) return 0;
    return 1;
}
RPCAPI int WINAPI UuidEqual(GUID *a, GUID *b, LONG *st)
{
    if (st) *st = RPC_S_OK_;
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static const char g_hex[] = "0123456789abcdef";
static void uuid_text(const GUID *u, char out[37])
{
    const BYTE *d4 = u->Data4;
    DWORD v[3] = { u->Data1, u->Data2, u->Data3 };
    int o = 0;
    for (int i = 7; i >= 0; i--) out[o++] = g_hex[(v[0] >> (4 * i)) & 15];
    out[o++] = '-';
    for (int i = 3; i >= 0; i--) out[o++] = g_hex[(v[1] >> (4 * i)) & 15];
    out[o++] = '-';
    for (int i = 3; i >= 0; i--) out[o++] = g_hex[(v[2] >> (4 * i)) & 15];
    out[o++] = '-';
    for (int i = 0; i < 8; i++) {
        if (i == 2) out[o++] = '-';
        out[o++] = g_hex[d4[i] >> 4];
        out[o++] = g_hex[d4[i] & 15];
    }
    out[o] = 0;
}
RPCAPI LONG WINAPI UuidToStringA(const GUID *u, BYTE **s)
{
    char *t = HeapAlloc(GetProcessHeap(), 0, 37);
    if (!t) return ERROR_NOT_ENOUGH_MEMORY;
    if (u) uuid_text(u, t); else lstrcpyA(t, "00000000-0000-0000-0000-000000000000");
    *s = (BYTE *)t;
    return RPC_S_OK_;
}
RPCAPI LONG WINAPI UuidToStringW(const GUID *u, WCHAR **s)
{
    char t[37];
    WCHAR *w = HeapAlloc(GetProcessHeap(), 0, 37 * sizeof(WCHAR));
    if (!w) return ERROR_NOT_ENOUGH_MEMORY;
    if (u) uuid_text(u, t); else lstrcpyA(t, "00000000-0000-0000-0000-000000000000");
    for (int i = 0; i < 37; i++) w[i] = (WCHAR)(BYTE)t[i];
    *s = w;
    return RPC_S_OK_;
}
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (NULL or empty: the nil UUID) */
static LONG uuid_parse(const char *s, GUID *u)
{
    if (!s || !*s) { ZeroMemory(u, sizeof(*u)); return RPC_S_OK_; }
    BYTE b[16];
    int k = 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return RPC_S_INVALID_STRING_UUID_; continue; }
        int h = hexval(s[i]), l;
        if (h < 0 || (l = hexval(s[++i])) < 0) return RPC_S_INVALID_STRING_UUID_;
        b[k++] = (BYTE)(h << 4 | l);
    }
    if (s[36]) return RPC_S_INVALID_STRING_UUID_;
    u->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 | b[3];
    u->Data2 = (WORD)(b[4] << 8 | b[5]);
    u->Data3 = (WORD)(b[6] << 8 | b[7]);
    for (int i = 0; i < 8; i++) u->Data4[i] = b[8 + i];
    return RPC_S_OK_;
}
RPCAPI LONG WINAPI UuidFromStringA(BYTE *s, GUID *u) { return uuid_parse((const char *)s, u); }
RPCAPI LONG WINAPI UuidFromStringW(WCHAR *s, GUID *u)
{
    char t[40];
    int i = 0;
    for (; s && s[i] && i < 39; i++) t[i] = s[i] < 0x80 ? (char)s[i] : '?';
    t[i] = 0;
    return uuid_parse(s ? t : 0, u);
}
RPCAPI LONG WINAPI RpcStringFreeA(BYTE **s) { if (s && *s) { HeapFree(GetProcessHeap(), 0, *s); *s = 0; } return RPC_S_OK_; }
RPCAPI LONG WINAPI RpcStringFreeW(WCHAR **s) { if (s && *s) { HeapFree(GetProcessHeap(), 0, *s); *s = 0; } return RPC_S_OK_; }
