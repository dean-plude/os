/*
 * dnsapi.dll — DNS queries.  Record lookups beyond addresses (which
 * ws2_32 resolves) are not available: every query finds no records.
 */
#include <windows.h>

#define DNSAPI __declspec(dllexport)
#define DNS_ERROR_RCODE_NAME_ERROR_ 9003

DNSAPI LONG WINAPI DnsQuery_A(LPCSTR name, WORD type, DWORD opts, PVOID extra, PVOID *results, PVOID *reserved)
{ (void)name; (void)type; (void)opts; (void)extra; if (results) *results = 0; if (reserved) *reserved = 0; return DNS_ERROR_RCODE_NAME_ERROR_; }
DNSAPI LONG WINAPI DnsQuery_W(PCWSTR name, WORD type, DWORD opts, PVOID extra, PVOID *results, PVOID *reserved)
{ (void)name; (void)type; (void)opts; (void)extra; if (results) *results = 0; if (reserved) *reserved = 0; return DNS_ERROR_RCODE_NAME_ERROR_; }
DNSAPI LONG WINAPI DnsQuery_UTF8(LPCSTR name, WORD type, DWORD opts, PVOID extra, PVOID *results, PVOID *reserved)
{ return DnsQuery_A(name, type, opts, extra, results, reserved); }
DNSAPI VOID WINAPI DnsFree(PVOID data, int type) { (void)data; (void)type; }
DNSAPI VOID WINAPI DnsRecordListFree(PVOID data, int type) { (void)data; (void)type; }

/* DnsQueryEx: the same answer (no records), synchronously or, with a
 * completion routine, on a thread of its own (DNS_REQUEST_PENDING), as
 * Windows completes an asynchronous query */
#define DNS_REQUEST_PENDING_ 9506
typedef struct {
    ULONG Version; LONG QueryStatus; ULONG64 QueryOptions; PVOID pQueryRecords; PVOID Reserved;
} DNS_QUERY_RESULT_;
typedef VOID (WINAPI *DNS_COMPLETION_)(PVOID ctx, DNS_QUERY_RESULT_ *result);
typedef struct {
    ULONG Version; PCWSTR QueryName; WORD QueryType; ULONG64 QueryOptions; PVOID pDnsServerList;
    ULONG InterfaceIndex; DNS_COMPLETION_ pQueryCompletionCallback; PVOID pQueryContext;
} DNS_QUERY_REQUEST_;
typedef struct { DNS_COMPLETION_ cb; PVOID ctx; DNS_QUERY_RESULT_ *result; } Pending;

static DWORD WINAPI complete_query(LPVOID p)
{
    Pending q = *(Pending *)p;
    HeapFree(GetProcessHeap(), 0, p);
    q.cb(q.ctx, q.result);
    return 0;
}

DNSAPI LONG WINAPI DnsQueryEx(DNS_QUERY_REQUEST_ *req, DNS_QUERY_RESULT_ *result, PVOID cancel)
{
    (void)cancel;
    if (!req || !result || !req->QueryName || (req->Version != 1 && req->Version != 3)) return ERROR_INVALID_PARAMETER;
    result->QueryOptions = req->QueryOptions;
    result->pQueryRecords = 0;
    result->QueryStatus = DnsQuery_W(req->QueryName, req->QueryType, (DWORD)req->QueryOptions, 0, 0, 0);
    if (!req->pQueryCompletionCallback) return result->QueryStatus;
    Pending *q = HeapAlloc(GetProcessHeap(), 0, sizeof(*q));
    if (!q) return ERROR_NOT_ENOUGH_MEMORY;
    q->cb = req->pQueryCompletionCallback; q->ctx = req->pQueryContext; q->result = result;
    HANDLE t = CreateThread(0, 0, complete_query, q, 0, 0);
    if (!t) { HeapFree(GetProcessHeap(), 0, q); return ERROR_NOT_ENOUGH_MEMORY; }
    CloseHandle(t);
    return DNS_REQUEST_PENDING_;
}
DNSAPI LONG WINAPI DnsCancelQuery(PVOID cancel) { (void)cancel; return ERROR_SUCCESS; }
