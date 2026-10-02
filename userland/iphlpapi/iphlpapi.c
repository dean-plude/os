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

/* -----------------------------------------------------------------------
 * The IP helper's newer calls (NET_LUID-based): one interface, eth0
 * ----------------------------------------------------------------------- */
typedef union { ULONG64 Value; } NET_LUID_;
IPHLPAPI DWORD WINAPI ConvertInterfaceIndexToLuid(ULONG index, NET_LUID_ *luid)
{
    if (!luid) return ERROR_INVALID_PARAMETER;
    if (index != 1) { luid->Value = 0; return ERROR_FILE_NOT_FOUND; }
    luid->Value = ((ULONG64)6 << 48) | ((ULONG64)1 << 24);       /* IfType 6 (Ethernet), NetLuidIndex 1 */
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceLuidToIndex(const NET_LUID_ *luid, ULONG *index)
{
    if (!luid || !index) return ERROR_INVALID_PARAMETER;
    *index = 1;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceLuidToNameW(const NET_LUID_ *luid, LPWSTR name, SIZE_T n)
{
    if (!luid || !name) return ERROR_INVALID_PARAMETER;
    if (n < 10) return ERROR_NOT_ENOUGH_MEMORY;
    lstrcpyW(name, L"ethernet_1");
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceLuidToNameA(const NET_LUID_ *luid, LPSTR name, SIZE_T n)
{
    if (!luid || !name) return ERROR_INVALID_PARAMETER;
    if (n < 11) return ERROR_NOT_ENOUGH_MEMORY;
    lstrcpyA(name, "ethernet_1");
    return NO_ERROR;
}
/* Change notifications: registered, never delivered */
IPHLPAPI DWORD WINAPI NotifyIpInterfaceChange(USHORT family, PVOID cb, PVOID ctx, BOOLEAN initial, HANDLE *h)
{
    (void)family; (void)cb; (void)ctx; (void)initial;
    if (!h) return ERROR_INVALID_PARAMETER;
    *h = (HANDLE)(ULONG_PTR)0x1F01;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI NotifyUnicastIpAddressChange(USHORT family, PVOID cb, PVOID ctx, BOOLEAN initial, HANDLE *h)
{
    return NotifyIpInterfaceChange(family, cb, ctx, initial, h);
}
IPHLPAPI DWORD WINAPI NotifyRouteChange2(USHORT family, PVOID cb, PVOID ctx, BOOLEAN initial, HANDLE *h)
{
    return NotifyIpInterfaceChange(family, cb, ctx, initial, h);
}
IPHLPAPI DWORD WINAPI CancelMibChangeNotify2(HANDLE h) { (void)h; return NO_ERROR; }
IPHLPAPI DWORD WINAPI GetBestRoute2(PVOID luid, ULONG index, const void *src, const void *dst, ULONG opts, PVOID route, PVOID best)
{
    (void)luid; (void)index; (void)src; (void)dst; (void)opts; (void)route; (void)best;
    return 1168;                                              /* ERROR_NOT_FOUND */
}

/* Interface tables (MIB_IF_TABLE2): empty, like the other tables here */
IPHLPAPI DWORD WINAPI GetIfTable2Ex(int level, PVOID *table)
{
    (void)level;
    if (!table) return ERROR_INVALID_PARAMETER;
    ULONG *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 16);
    if (!t) return ERROR_NOT_ENOUGH_MEMORY;
    *table = t;                                       /* NumEntries = 0 */
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI GetIfTable2(PVOID *table) { return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetIpInterfaceTable(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetUnicastIpAddressTable(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetIpForwardTable2(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetIpNetTable2(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetAnycastIpAddressTable(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI DWORD WINAPI GetMulticastIpAddressTable(USHORT family, PVOID *table) { (void)family; return GetIfTable2Ex(0, table); }
IPHLPAPI VOID WINAPI FreeMibTable(PVOID table) { if (table) HeapFree(GetProcessHeap(), 0, table); }
