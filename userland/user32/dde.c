/*
 * dde.c — Dynamic Data Exchange, and the process layout.  No DDE server
 * runs here: the management library initializes (so a program's DDE
 * client or server object constructs), string handles work, and every
 * conversation fails to connect (DMLERR_NO_CONV_ESTABLISHED).
 */
#include "u32.h"

#define DMLERR_NO_ERROR             0
#define DMLERR_INVALIDPARAMETER     0x4006
#define DMLERR_NO_CONV_ESTABLISHED  0x400A
#define DMLERR_DLL_NOT_INITIALIZED  0x4004
#define DMLERR_NOTPROCESSED         0x400C
#define DMLERR_SYS_ERROR            0x4011

typedef struct { DWORD inst; UINT err; } DdeInst;
#define DDE_INST_MAX 32
static DdeInst g_dde[DDE_INST_MAX];

static DdeInst *dde_of(DWORD inst)
{
    for (int i = 0; i < DDE_INST_MAX; i++) if (g_dde[i].inst && g_dde[i].inst == inst) return &g_dde[i];
    return 0;
}
static void dde_err(DWORD inst, UINT e) { DdeInst *d = dde_of(inst); if (d) d->err = e; }

USERAPI UINT DdeInitializeW(LPDWORD inst, PVOID cb, DWORD flags, DWORD reserved)
{
    (void)cb; (void)flags;
    if (!inst || reserved) return DMLERR_INVALIDPARAMETER;
    if (*inst) return dde_of(*inst) ? DMLERR_NO_ERROR : DMLERR_INVALIDPARAMETER;   /* reinitialize */
    for (int i = 0; i < DDE_INST_MAX; i++) if (!g_dde[i].inst) {
        g_dde[i].inst = 0x1000 + (DWORD)i * 0x10;
        g_dde[i].err = 0;
        *inst = g_dde[i].inst;
        return DMLERR_NO_ERROR;
    }
    return DMLERR_SYS_ERROR;
}
USERAPI UINT DdeInitializeA(LPDWORD inst, PVOID cb, DWORD flags, DWORD reserved) { return DdeInitializeW(inst, cb, flags, reserved); }
USERAPI BOOL DdeUninitialize(DWORD inst) { DdeInst *d = dde_of(inst); if (!d) return FALSE; d->inst = 0; return TRUE; }
USERAPI UINT DdeGetLastError(DWORD inst) { DdeInst *d = dde_of(inst); return d ? d->err : DMLERR_DLL_NOT_INITIALIZED; }

/* String handles: the strings themselves, copied on the heap */
USERAPI HANDLE DdeCreateStringHandleW(DWORD inst, LPCWSTR s, int cp)
{
    (void)cp;
    if (!dde_of(inst) || !s) { dde_err(inst, DMLERR_INVALIDPARAMETER); return 0; }
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    WCHAR *c = HeapAlloc(GetProcessHeap(), 0, n);
    if (c) memcpy(c, s, n);
    return c;
}
USERAPI HANDLE DdeCreateStringHandleA(DWORD inst, LPCSTR s, int cp)
{
    if (!dde_of(inst) || !s) { dde_err(inst, DMLERR_INVALIDPARAMETER); return 0; }
    int n = MultiByteToWideChar(cp == 1200 ? CP_ACP : CP_ACP, 0, s, -1, 0, 0);
    WCHAR *c = HeapAlloc(GetProcessHeap(), 0, (n ? n : 1) * sizeof(WCHAR));
    if (c) { if (n) MultiByteToWideChar(CP_ACP, 0, s, -1, c, n); else c[0] = 0; }
    return c;
}
USERAPI BOOL DdeFreeStringHandle(DWORD inst, HANDLE h) { (void)inst; if (h) HeapFree(GetProcessHeap(), 0, h); return h != 0; }
USERAPI BOOL DdeKeepStringHandle(DWORD inst, HANDLE h) { (void)inst; return h != 0; }
USERAPI DWORD DdeQueryStringW(DWORD inst, HANDLE h, LPWSTR buf, DWORD n, int cp)
{
    (void)inst; (void)cp;
    if (!h) return 0;
    DWORD len = (DWORD)wcslen((WCHAR *)h);
    if (!buf) return len;
    if (!n) return 0;
    if (len >= n) len = n - 1;
    memcpy(buf, h, len * sizeof(WCHAR)); buf[len] = 0;
    return len;
}
USERAPI DWORD DdeQueryStringA(DWORD inst, HANDLE h, LPSTR buf, DWORD n, int cp)
{
    (void)inst; (void)cp;
    if (!h) return 0;
    int len = WideCharToMultiByte(CP_ACP, 0, h, -1, 0, 0, 0, 0);
    if (len) len--;
    if (!buf) return (DWORD)len;
    if (!n) return 0;
    if ((DWORD)len >= n) len = (int)n - 1;
    WideCharToMultiByte(CP_ACP, 0, h, -1, buf, len, 0, 0); buf[len] = 0;
    return (DWORD)len;
}
USERAPI int DdeCmpStringHandles(HANDLE a, HANDLE b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    int r = lstrcmpiW(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

/* Data handles: a length-prefixed copy */
USERAPI HANDLE DdeCreateDataHandle(DWORD inst, LPBYTE src, DWORD n, DWORD off, HANDLE item, UINT fmt, UINT flags)
{
    (void)item; (void)fmt; (void)flags;
    if (!dde_of(inst)) return 0;
    DWORD *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DWORD) + n);
    if (!d) return 0;
    d[0] = n;
    if (src && n > off) memcpy((BYTE *)(d + 1) + off, src + off, n - off);
    return d;
}
USERAPI BOOL DdeFreeDataHandle(HANDLE h) { if (h) HeapFree(GetProcessHeap(), 0, h); return h != 0; }
USERAPI DWORD DdeGetData(HANDLE h, LPBYTE buf, DWORD n, DWORD off)
{
    DWORD *d = h;
    if (!d) return 0;
    if (!buf) return d[0];
    if (off >= d[0]) return 0;
    DWORD k = d[0] - off;
    if (k > n) k = n;
    memcpy(buf, (BYTE *)(d + 1) + off, k);
    return k;
}
USERAPI LPBYTE DdeAccessData(HANDLE h, LPDWORD n) { DWORD *d = h; if (!d) return 0; if (n) *n = d[0]; return (LPBYTE)(d + 1); }
USERAPI BOOL DdeUnaccessData(HANDLE h) { return h != 0; }
USERAPI HANDLE DdeAddData(HANDLE h, LPBYTE src, DWORD n, DWORD off)
{
    DWORD *d = h;
    if (!d) return 0;
    if (off + n > d[0]) {
        DWORD *nd = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, d, sizeof(DWORD) + off + n);
        if (!nd) return 0;
        d = nd; d[0] = off + n;
    }
    if (src) memcpy((BYTE *)(d + 1) + off, src, n);
    return d;
}

/* Conversations: no server answers, so none opens */
USERAPI HANDLE DdeConnect(DWORD inst, HANDLE service, HANDLE topic, PVOID ctx)
{
    (void)service; (void)topic; (void)ctx;
    dde_err(inst, DMLERR_NO_CONV_ESTABLISHED);
    return 0;
}
USERAPI HANDLE DdeConnectList(DWORD inst, HANDLE service, HANDLE topic, HANDLE prev, PVOID ctx)
{
    (void)service; (void)topic; (void)prev; (void)ctx;
    dde_err(inst, DMLERR_NO_CONV_ESTABLISHED);
    return 0;
}
USERAPI HANDLE DdeQueryNextServer(HANDLE list, HANDLE prev) { (void)list; (void)prev; return 0; }
USERAPI BOOL DdeDisconnectList(HANDLE list) { (void)list; return TRUE; }
USERAPI HANDLE DdeReconnect(HANDLE conv) { (void)conv; return 0; }
USERAPI BOOL DdeDisconnect(HANDLE conv) { (void)conv; return TRUE; }
USERAPI HANDLE DdeClientTransaction(LPBYTE data, DWORD n, HANDLE conv, HANDLE item, UINT fmt, UINT type, DWORD timeout, LPDWORD result)
{
    (void)data; (void)n; (void)conv; (void)item; (void)fmt; (void)type; (void)timeout;
    if (result) *result = 0;
    return 0;
}
USERAPI BOOL DdeAbandonTransaction(DWORD inst, HANDLE conv, DWORD id) { (void)inst; (void)conv; (void)id; return TRUE; }
USERAPI BOOL DdePostAdvise(DWORD inst, HANDLE topic, HANDLE item) { (void)topic; (void)item; return dde_of(inst) != 0; }
USERAPI BOOL DdeEnableCallback(DWORD inst, HANDLE conv, UINT cmd) { (void)conv; (void)cmd; return dde_of(inst) != 0; }
USERAPI BOOL DdeImpersonateClient(HANDLE conv) { (void)conv; return FALSE; }
USERAPI HANDLE DdeNameService(DWORD inst, HANDLE s1, HANDLE s2, UINT cmd)
{
    (void)s1; (void)s2; (void)cmd;
    return dde_of(inst) ? (HANDLE)1 : 0;                     /* registered: no client will find it */
}
USERAPI BOOL DdeSetUserHandle(HANDLE conv, DWORD id, DWORD_PTR user) { (void)conv; (void)id; (void)user; return FALSE; }
USERAPI UINT DdeQueryConvInfo(HANDLE conv, DWORD id, PVOID info) { (void)conv; (void)id; (void)info; return 0; }
USERAPI BOOL DdeSetQualityOfService(HWND client, const void *qos, void *prev) { (void)client; (void)qos; (void)prev; return TRUE; }

/* Windows read left to right */
USERAPI BOOL GetProcessDefaultLayout(DWORD *layout) { if (!layout) return FALSE; *layout = 0; return TRUE; }
USERAPI BOOL SetProcessDefaultLayout(DWORD layout) { (void)layout; return TRUE; }
