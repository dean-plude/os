/*
 * iphlpapi.dll — IP helper: the host's network parameters and tables.
 *
 * GetNetworkParams gives the computer's name (no DNS domain).  The
 * adapter and connection tables are empty: programs find no interfaces to
 * list, the answer Windows gives on a machine without IP configured.
 */
#include <windows.h>

#define IPHLPAPI __declspec(dllexport)
#define ERROR_NO_DATA_ 232
#define ERROR_BUFFER_OVERFLOW_ 111
#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#define ERROR_INVALID_DATA_ 13

typedef struct IP_ADDR_STRING_ { struct IP_ADDR_STRING_ *Next; char IpAddress[16], IpMask[16]; DWORD Context; } IP_ADDR_STRING_;
typedef struct {
    char HostName[132], DomainName[132];
    IP_ADDR_STRING_ *CurrentDnsServer;
    IP_ADDR_STRING_ DnsServerList;
    UINT NodeType;
    char ScopeId[260];
    UINT EnableRouting, EnableProxy, EnableDns;
} FIXED_INFO_;

void *memset(void *d, int c, size_t n);

IPHLPAPI DWORD WINAPI GetNetworkParams(FIXED_INFO_ *info, PULONG size)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!info || *size < sizeof(FIXED_INFO_)) { *size = sizeof(FIXED_INFO_); return ERROR_BUFFER_OVERFLOW_; }
    memset(info, 0, sizeof(*info));
    DWORD n = sizeof(info->HostName);
    if (!GetComputerNameA(info->HostName, &n)) lstrcpyA(info->HostName, "NOVA-PC");
    info->NodeType = 1;                                  /* BROADCAST */
    return NO_ERROR;
}

/* No adapters, addresses or connections to report */
IPHLPAPI ULONG WINAPI GetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PVOID addrs, PULONG size)
{
    (void)family; (void)flags; (void)reserved; (void)addrs;
    if (!size) return ERROR_INVALID_PARAMETER;
    return ERROR_NO_DATA_;
}
IPHLPAPI DWORD WINAPI GetAdaptersInfo(PVOID info, PULONG size) { (void)info; if (!size) return ERROR_INVALID_PARAMETER; return ERROR_NO_DATA_; }
IPHLPAPI DWORD WINAPI GetIfEntry(PVOID row) { (void)row; return ERROR_INVALID_DATA_; }
IPHLPAPI DWORD WINAPI GetNumberOfInterfaces(PDWORD n) { *n = 0; return NO_ERROR; }

/* An empty MIB table: just its entry count */
static DWORD empty_table(PVOID table, PULONG size)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!table || *size < sizeof(DWORD)) { *size = sizeof(DWORD); return ERROR_INSUFFICIENT_BUFFER; }
    *(DWORD *)table = 0;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI GetIpAddrTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetIpForwardTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetTcpTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetTcp6Table(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetUdpTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetIfTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetExtendedTcpTable(PVOID t, PDWORD size, BOOL order, ULONG af, int cls, ULONG r)
{ (void)order; (void)af; (void)cls; (void)r; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetExtendedUdpTable(PVOID t, PDWORD size, BOOL order, ULONG af, int cls, ULONG r)
{ (void)order; (void)af; (void)cls; (void)r; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetBestInterface(DWORD addr, PDWORD index) { (void)addr; (void)index; return ERROR_NO_DATA_; }
IPHLPAPI DWORD WINAPI NotifyAddrChange(PHANDLE h, LPOVERLAPPED o) { (void)h; (void)o; return ERROR_NOT_SUPPORTED; }

/* Interface names and indexes: the one interface is "eth0", index 1 */
IPHLPAPI ULONG WINAPI if_nametoindex(const char *name) { return name && !lstrcmpA(name, "eth0") ? 1 : 0; }
IPHLPAPI char *WINAPI if_indextoname(ULONG index, char *name)
{
    if (index != 1 || !name) return 0;
    lstrcpyA(name, "eth0");
    return name;
}
