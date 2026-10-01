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
DNSAPI VOID WINAPI DnsFree(PVOID data, int type) { (void)data; (void)type; }
DNSAPI VOID WINAPI DnsRecordListFree(PVOID data, int type) { (void)data; (void)type; }
