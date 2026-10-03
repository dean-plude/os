/*
 * dde.c — Dynamic Data Exchange.  No program on NovaOS serves DDE, so the
 * DDEML client finds no conversation (DdeConnect gives none; programs such
 * as SumatraPDF then start their own window instead of handing the file to
 * a running copy).  String handles and the WM_DDE_* lParam packing work,
 * so a program's own DDE window code runs as written.
 */
#include "u32.h"

#define DMLERR_NO_ERROR             0
#define DMLERR_INVALIDPARAMETER     0x4006
#define DMLERR_NO_CONV_ESTABLISHED  0x400A
#define WM_DDE_ACK      0x03E4
#define WM_DDE_ADVISE   0x03E2
#define WM_DDE_DATA     0x03E5
#define WM_DDE_POKE     0x03E7

static volatile LONG g_inst;
static UINT g_dde_error;

typedef struct DdeStr { LONG refs; WCHAR s[1]; } DdeStr;

USERAPI UINT DdeInitializeW(LPDWORD inst, void *cb, DWORD cmd, DWORD res)
{
    (void)cb; (void)cmd; (void)res;
    if (!inst) return DMLERR_INVALIDPARAMETER;
    if (!*inst) *inst = (DWORD)InterlockedIncrement(&g_inst);
    return DMLERR_NO_ERROR;
}
USERAPI UINT DdeInitializeA(LPDWORD inst, void *cb, DWORD cmd, DWORD res) { return DdeInitializeW(inst, cb, cmd, res); }
USERAPI BOOL DdeUninitialize(DWORD inst) { (void)inst; return TRUE; }
USERAPI UINT DdeGetLastError(DWORD inst) { (void)inst; UINT e = g_dde_error; g_dde_error = 0; return e; }

USERAPI HANDLE DdeCreateStringHandleW(DWORD inst, LPCWSTR s, int cp)
{
    (void)inst; (void)cp;
    if (!s) return NULL;
    size_t n = wcslen(s);
    DdeStr *d = HeapAlloc(GetProcessHeap(), 0, sizeof *d + n * sizeof(WCHAR));
    if (!d) return NULL;
    d->refs = 1;
    memcpy(d->s, s, (n + 1) * sizeof(WCHAR));
    return d;
}
USERAPI HANDLE DdeCreateStringHandleA(DWORD inst, LPCSTR s, int cp)
{
    if (!s) return NULL;
    WCHAR w[256];
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, 256);
    w[255] = 0;
    return DdeCreateStringHandleW(inst, w, cp);
}
USERAPI BOOL DdeFreeStringHandle(DWORD inst, HANDLE h)
{
    (void)inst;
    DdeStr *d = h;
    if (d && !InterlockedDecrement(&d->refs)) HeapFree(GetProcessHeap(), 0, d);
    return d != NULL;
}
USERAPI BOOL DdeKeepStringHandle(DWORD inst, HANDLE h) { (void)inst; if (!h) return FALSE; InterlockedIncrement(&((DdeStr *)h)->refs); return TRUE; }
USERAPI DWORD DdeQueryStringW(DWORD inst, HANDLE h, LPWSTR out, DWORD cch, int cp)
{
    (void)inst; (void)cp;
    if (!h) return 0;
    DWORD n = (DWORD)wcslen(((DdeStr *)h)->s);
    if (out && cch) { DWORD k = n < cch - 1 ? n : cch - 1; memcpy(out, ((DdeStr *)h)->s, k * sizeof(WCHAR)); out[k] = 0; }
    return n;
}
USERAPI int DdeCmpStringHandles(HANDLE a, HANDLE b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    int c = lstrcmpiW(((DdeStr *)a)->s, ((DdeStr *)b)->s);
    return c < 0 ? -1 : c > 0;
}

/* conversations: there is never a server to talk to */
USERAPI HANDLE DdeConnect(DWORD inst, HANDLE service, HANDLE topic, void *ctx)
{ (void)inst; (void)service; (void)topic; (void)ctx; g_dde_error = DMLERR_NO_CONV_ESTABLISHED; return NULL; }
USERAPI HANDLE DdeConnectList(DWORD inst, HANDLE service, HANDLE topic, HANDLE list, void *ctx)
{ (void)inst; (void)service; (void)topic; (void)list; (void)ctx; g_dde_error = DMLERR_NO_CONV_ESTABLISHED; return NULL; }
USERAPI BOOL DdeDisconnect(HANDLE conv) { (void)conv; return TRUE; }
USERAPI HANDLE DdeClientTransaction(LPBYTE data, DWORD n, HANDLE conv, HANDLE item, UINT fmt, UINT type, DWORD timeout, LPDWORD result)
{
    (void)data; (void)n; (void)conv; (void)item; (void)fmt; (void)type; (void)timeout;
    if (result) *result = 0;
    g_dde_error = DMLERR_NO_CONV_ESTABLISHED;
    return NULL;
}
USERAPI HANDLE DdeNameService(DWORD inst, HANDLE s1, HANDLE s2, UINT cmd) { (void)inst; (void)s1; (void)s2; (void)cmd; return (HANDLE)1; }

/* data handles: a length and the bytes */
typedef struct { DWORD n; BYTE d[1]; } DdeData;
USERAPI HANDLE DdeCreateDataHandle(DWORD inst, LPBYTE src, DWORD n, DWORD off, HANDLE item, UINT fmt, UINT cmd)
{
    (void)inst; (void)item; (void)fmt; (void)cmd;
    DdeData *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *h + n);
    if (!h) return NULL;
    h->n = n;
    if (src) memcpy(h->d, src + off, n);
    return h;
}
USERAPI BOOL DdeFreeDataHandle(HANDLE h) { if (!h) return FALSE; HeapFree(GetProcessHeap(), 0, h); return TRUE; }
USERAPI LPBYTE DdeAccessData(HANDLE h, LPDWORD n) { if (!h) return NULL; if (n) *n = ((DdeData *)h)->n; return ((DdeData *)h)->d; }
USERAPI BOOL DdeUnaccessData(HANDLE h) { return h != NULL; }
USERAPI DWORD DdeGetData(HANDLE h, LPBYTE out, DWORD n, DWORD off)
{
    if (!h) return 0;
    DdeData *d = h;
    if (!out) return d->n;
    if (off >= d->n) return 0;
    DWORD k = d->n - off < n ? d->n - off : n;
    memcpy(out, d->d + off, k);
    return k;
}

/* the lParam of WM_DDE_ACK, _ADVISE, _DATA and _POKE: two values packed in a block */
static BOOL packed(UINT msg) { return msg == WM_DDE_ACK || msg == WM_DDE_ADVISE || msg == WM_DDE_DATA || msg == WM_DDE_POKE; }
USERAPI LPARAM PackDDElParam(UINT msg, UINT_PTR lo, UINT_PTR hi)
{
    if (!packed(msg)) return (LPARAM)MAKELONG(lo, hi);
    UINT_PTR *p = HeapAlloc(GetProcessHeap(), 0, 2 * sizeof *p);
    if (!p) return 0;
    p[0] = lo;
    p[1] = hi;
    return (LPARAM)p;
}
USERAPI BOOL UnpackDDElParam(UINT msg, LPARAM lp, UINT_PTR *lo, UINT_PTR *hi)
{
    if (!packed(msg)) {
        if (lo) *lo = LOWORD(lp);
        if (hi) *hi = HIWORD(lp);
        return TRUE;
    }
    if (!lp) { if (lo) *lo = 0; if (hi) *hi = 0; return FALSE; }
    if (lo) *lo = ((UINT_PTR *)lp)[0];
    if (hi) *hi = ((UINT_PTR *)lp)[1];
    return TRUE;
}
USERAPI BOOL FreeDDElParam(UINT msg, LPARAM lp)
{
    if (packed(msg) && lp) HeapFree(GetProcessHeap(), 0, (void *)lp);
    return TRUE;
}
USERAPI LPARAM ReuseDDElParam(LPARAM lp, UINT in, UINT out, UINT_PTR lo, UINT_PTR hi)
{
    FreeDDElParam(in, lp);
    return PackDDElParam(out, lo, hi);
}
USERAPI BOOL ImpersonateDdeClientWindow(HWND client, HWND server) { (void)client; (void)server; return TRUE; }

/* WinHelp: there is no Windows Help viewer (help files are .chm now) */
USERAPI BOOL WinHelpW(HWND w, LPCWSTR file, UINT cmd, ULONG_PTR data)
{ (void)w; (void)file; (void)cmd; (void)data; SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
USERAPI BOOL WinHelpA(HWND w, LPCSTR file, UINT cmd, ULONG_PTR data)
{ (void)w; (void)file; (void)cmd; (void)data; SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
