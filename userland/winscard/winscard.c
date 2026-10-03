/*
 * winscard.dll — the smart card API.  NovaOS has no smart card service or
 * readers, so it answers as Windows does when the Smart Card service is
 * not running: SCARD_E_NO_SERVICE.  Password managers and similar
 * programs import it for hardware keys and start fine without one.
 */
#include <windows.h>

#define SCARDAPI __declspec(dllexport)
#define SCARD_E_INVALID_HANDLE ((LONG)0x80100003)
#define SCARD_E_NO_SERVICE     ((LONG)0x8010001D)

typedef struct { DWORD dwProtocol, cbPciLength; } IoRequest;
SCARDAPI const IoRequest g_rgSCardT0Pci = { 1, sizeof(IoRequest) };
SCARDAPI const IoRequest g_rgSCardT1Pci = { 2, sizeof(IoRequest) };
SCARDAPI const IoRequest g_rgSCardRawPci = { 0x10000, sizeof(IoRequest) };

SCARDAPI LONG WINAPI SCardEstablishContext(DWORD scope, const void *r1, const void *r2, ULONG_PTR *ctx)
{
    (void)scope; (void)r1; (void)r2;
    if (ctx) *ctx = 0;
    return SCARD_E_NO_SERVICE;
}
SCARDAPI LONG WINAPI SCardReleaseContext(ULONG_PTR ctx) { (void)ctx; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardIsValidContext(ULONG_PTR ctx) { (void)ctx; return SCARD_E_INVALID_HANDLE; }

static LONG no_readers(LPDWORD n)
{
    if (n) *n = 0;
    return SCARD_E_NO_SERVICE;
}
SCARDAPI LONG WINAPI SCardListReadersA(ULONG_PTR ctx, LPCSTR groups, LPSTR out, LPDWORD n)
{ (void)ctx; (void)groups; (void)out; return no_readers(n); }
SCARDAPI LONG WINAPI SCardListReadersW(ULONG_PTR ctx, LPCWSTR groups, LPWSTR out, LPDWORD n)
{ (void)ctx; (void)groups; (void)out; return no_readers(n); }
SCARDAPI LONG WINAPI SCardListReaderGroupsA(ULONG_PTR ctx, LPSTR out, LPDWORD n) { (void)ctx; (void)out; return no_readers(n); }
SCARDAPI LONG WINAPI SCardListReaderGroupsW(ULONG_PTR ctx, LPWSTR out, LPDWORD n) { (void)ctx; (void)out; return no_readers(n); }
SCARDAPI LONG WINAPI SCardFreeMemory(ULONG_PTR ctx, LPCVOID mem) { (void)ctx; (void)mem; return 0; }

static LONG no_card(ULONG_PTR *card, LPDWORD proto)
{
    if (card) *card = 0;
    if (proto) *proto = 0;
    return SCARD_E_NO_SERVICE;
}
SCARDAPI LONG WINAPI SCardConnectA(ULONG_PTR ctx, LPCSTR reader, DWORD share, DWORD protos, ULONG_PTR *card, LPDWORD proto)
{ (void)ctx; (void)reader; (void)share; (void)protos; return no_card(card, proto); }
SCARDAPI LONG WINAPI SCardConnectW(ULONG_PTR ctx, LPCWSTR reader, DWORD share, DWORD protos, ULONG_PTR *card, LPDWORD proto)
{ (void)ctx; (void)reader; (void)share; (void)protos; return no_card(card, proto); }
SCARDAPI LONG WINAPI SCardReconnect(ULONG_PTR card, DWORD share, DWORD protos, DWORD init, LPDWORD proto)
{ (void)card; (void)share; (void)protos; (void)init; if (proto) *proto = 0; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardDisconnect(ULONG_PTR card, DWORD how) { (void)card; (void)how; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardBeginTransaction(ULONG_PTR card) { (void)card; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardEndTransaction(ULONG_PTR card, DWORD how) { (void)card; (void)how; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardStatusA(ULONG_PTR card, LPSTR names, LPDWORD nlen, LPDWORD state, LPDWORD proto, LPBYTE atr, LPDWORD atrlen)
{
    (void)card; (void)names; (void)state; (void)proto; (void)atr;
    if (nlen) *nlen = 0;
    if (atrlen) *atrlen = 0;
    return SCARD_E_INVALID_HANDLE;
}
SCARDAPI LONG WINAPI SCardStatusW(ULONG_PTR card, LPWSTR names, LPDWORD nlen, LPDWORD state, LPDWORD proto, LPBYTE atr, LPDWORD atrlen)
{ return SCardStatusA(card, (LPSTR)names, nlen, state, proto, atr, atrlen); }
SCARDAPI LONG WINAPI SCardTransmit(ULONG_PTR card, const IoRequest *send_pci, const BYTE * send, DWORD nsend,
                                   IoRequest *recv_pci, LPBYTE recv, LPDWORD nrecv)
{
    (void)card; (void)send_pci; (void)send; (void)nsend; (void)recv_pci; (void)recv;
    if (nrecv) *nrecv = 0;
    return SCARD_E_INVALID_HANDLE;
}
SCARDAPI LONG WINAPI SCardControl(ULONG_PTR card, DWORD code, LPCVOID in, DWORD nin, LPVOID out, DWORD nout, LPDWORD n)
{ (void)card; (void)code; (void)in; (void)nin; (void)out; (void)nout; if (n) *n = 0; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardGetAttrib(ULONG_PTR card, DWORD id, LPBYTE attr, LPDWORD n)
{ (void)card; (void)id; (void)attr; if (n) *n = 0; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardSetAttrib(ULONG_PTR card, DWORD id, const BYTE * attr, DWORD n)
{ (void)card; (void)id; (void)attr; (void)n; return SCARD_E_INVALID_HANDLE; }
SCARDAPI LONG WINAPI SCardGetStatusChangeA(ULONG_PTR ctx, DWORD timeout, void *readers, DWORD n)
{ (void)ctx; (void)timeout; (void)readers; (void)n; return SCARD_E_NO_SERVICE; }
SCARDAPI LONG WINAPI SCardGetStatusChangeW(ULONG_PTR ctx, DWORD timeout, void *readers, DWORD n)
{ (void)ctx; (void)timeout; (void)readers; (void)n; return SCARD_E_NO_SERVICE; }
SCARDAPI LONG WINAPI SCardCancel(ULONG_PTR ctx) { (void)ctx; return SCARD_E_INVALID_HANDLE; }

/* (the service's "started" event: never signalled, as no service runs) */
SCARDAPI HANDLE WINAPI SCardAccessStartedEvent(void)
{
    static HANDLE ev;
    if (!ev) ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    return ev;
}
SCARDAPI void WINAPI SCardReleaseStartedEvent(void) {}
