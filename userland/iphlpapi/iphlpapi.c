/*
 * iphlpapi.dll — IP helper: the host's network parameters and tables.
 *
 * GetNetworkParams gives the computer's name (no DNS domain).  The
 * adapter tables are empty: programs find no interfaces to list, the
 * answer Windows gives on a machine without IP configured.  The TCP
 * connection tables are real (the kernel's sockets, with the process that
 * owns each): a local server finds which process is calling it this way.
 */
#include <windows.h>
#include <winternl.h>

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
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

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
IPHLPAPI DWORD WINAPI GetUdpTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetIfTable(PVOID t, PULONG size, BOOL order) { (void)order; return empty_table(t, size); }
IPHLPAPI DWORD WINAPI GetExtendedUdpTable(PVOID t, PDWORD size, BOOL order, ULONG af, int cls, ULONG r)
{ (void)order; (void)af; (void)cls; (void)r; return empty_table(t, size); }
/* -----------------------------------------------------------------------
 * The TCP connection tables (GetTcpTable, GetTcp6Table, GetExtendedTcpTable)
 * ----------------------------------------------------------------------- */
/* A row from the kernel (NtNovaSockCtl op 14): addresses as Winsock's,
 * ports in network order, a MIB_TCP_STATE and the owning process */
typedef struct { USHORT family, port; BYTE addr[16]; ULONG scope; } KAddr;
typedef struct { KAddr local, remote; DWORD state, owner; } KTcpRow;

#define MIB_TCP_STATE_LISTEN_ 2
#define AF_INET_  2
#define AF_INET6_ 23
enum { TCP_BASIC_LISTENER, TCP_BASIC_CONNECTIONS, TCP_BASIC_ALL, TCP_PID_LISTENER, TCP_PID_CONNECTIONS,
       TCP_PID_ALL, TCP_MODULE_LISTENER, TCP_MODULE_CONNECTIONS, TCP_MODULE_ALL };

/* The kernel's rows for one family (2 or 23), filtered by the class's
 * listener / connection choice, sorted when @order asks; HeapFree them */
static KTcpRow *tcp_rows(ULONG af, int which, BOOL order, DWORD *count)
{
    KTcpRow *all = NULL;
    LONG_PTR n = NtNovaSockCtl(0, 14, 0, NULL);
    for (int tries = 0; tries < 4 && n > 0; tries++) {
        HeapFree(GetProcessHeap(), 0, all);
        all = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(n + 8) * sizeof(KTcpRow));
        if (!all) { n = 0; break; }
        LONG_PTR got = NtNovaSockCtl(0, 14, (ULONG_PTR)(n + 8), all);
        if (got <= n + 8) { n = got; break; }
        n = got;                                    /* (more sockets came meanwhile) */
    }
    DWORD k = 0;
    for (LONG_PTR i = 0; i < n; i++) {
        KTcpRow *r = &all[i];
        BOOL listening = r->state == MIB_TCP_STATE_LISTEN_;
        if (r->local.family != af || (which == 0 && !listening) || (which == 1 && listening)) continue;
        all[k++] = *r;
    }
    /* by local address and port, then remote address and port */
    for (DWORD i = 1; order && i < k; i++)
        for (DWORD j = i; j > 0; j--) {
            KTcpRow *a = &all[j - 1], *b = &all[j];
            int c = memcmp(a->local.addr, b->local.addr, 16);
            if (!c) c = (int)((a->local.port >> 8 | a->local.port << 8) & 0xFFFF) - (int)((b->local.port >> 8 | b->local.port << 8) & 0xFFFF);
            if (!c) c = memcmp(a->remote.addr, b->remote.addr, 16);
            if (!c) c = (int)((a->remote.port >> 8 | a->remote.port << 8) & 0xFFFF) - (int)((b->remote.port >> 8 | b->remote.port << 8) & 0xFFFF);
            if (c <= 0) break;
            KTcpRow t = *a; *a = *b; *b = t;
        }
    *count = k;
    return all;
}

static DWORD tcp_table(PVOID t, PDWORD size, BOOL order, ULONG af, int cls)
{
    if (!size || (af != AF_INET_ && af != AF_INET6_) || cls < -1 || cls > TCP_MODULE_ALL) return ERROR_INVALID_PARAMETER;
    if (af == AF_INET6_ && cls >= 0 && cls <= TCP_BASIC_ALL) return ERROR_NOT_SUPPORTED;
    /* row sizes: MIB_TCPROW(_OWNER_PID/_OWNER_MODULE), MIB_TCP6ROW_OWNER_PID/_MODULE, MIB_TCP6ROW (cls -1) */
    static const DWORD v4[3] = { 20, 24, 160 }, v6[3] = { 52, 56, 192 };
    int kind = cls < 0 ? 0 : cls / 3;
    DWORD row = af == AF_INET_ ? v4[kind] : v6[kind], head = kind == 2 ? 8 : 4;
    DWORD n;
    KTcpRow *rows = tcp_rows(af, cls % 3, order, &n);
    DWORD need = head + n * row;
    if (!t || *size < need) {
        HeapFree(GetProcessHeap(), 0, rows);
        *size = need + 4 * row;                      /* (room for a few more) */
        return ERROR_INSUFFICIENT_BUFFER;
    }
    memset(t, 0, need);
    *(DWORD *)t = n;
    for (DWORD i = 0; i < n; i++) {
        BYTE *o = (BYTE *)t + head + i * row;
        KTcpRow *r = &rows[i];
        DWORD *d = (DWORD *)o;
        if (af == AF_INET_) {                         /* state, local addr, port, remote addr, port[, pid] */
            d[0] = r->state;
            memcpy(&d[1], r->local.addr, 4); d[2] = r->local.port;
            memcpy(&d[3], r->remote.addr, 4); d[4] = r->remote.port;
            if (kind) d[5] = r->owner;
        } else if (kind) {                           /* MIB_TCP6ROW_OWNER_*: local, scope, port, remote, scope, port, state, pid */
            memcpy(o, r->local.addr, 16); d[4] = r->local.scope; d[5] = r->local.port;
            memcpy(o + 24, r->remote.addr, 16); d[10] = r->remote.scope; d[11] = r->remote.port;
            d[12] = r->state; d[13] = r->owner;
        } else {                                     /* MIB_TCP6ROW: state, local, scope, port, remote, scope, port */
            d[0] = r->state;
            memcpy(o + 4, r->local.addr, 16); d[5] = r->local.scope; d[6] = r->local.port;
            memcpy(o + 28, r->remote.addr, 16); d[11] = r->remote.scope; d[12] = r->remote.port;
        }
    }
    HeapFree(GetProcessHeap(), 0, rows);
    return NO_ERROR;
}

IPHLPAPI DWORD WINAPI GetExtendedTcpTable(PVOID t, PDWORD size, BOOL order, ULONG af, int cls, ULONG r)
{
    if (r) return ERROR_INVALID_PARAMETER;
    return tcp_table(t, size, order, af, cls);
}
IPHLPAPI DWORD WINAPI GetTcpTable(PVOID t, PULONG size, BOOL order) { return tcp_table(t, size, order, AF_INET_, TCP_BASIC_ALL); }
IPHLPAPI DWORD WINAPI GetTcp6Table(PVOID t, PULONG size, BOOL order) { return tcp_table(t, size, order, AF_INET6_, -1); }

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

/* The interface toward an address (sockaddr form): no route is known */
IPHLPAPI DWORD WINAPI GetBestInterfaceEx(const void *addr, PDWORD index) { (void)addr; (void)index; return ERROR_NO_DATA_; }

/* The interface's other names: "ethernet_1" (its NDIS-style name) and a
 * GUID of its own, both naming eth0's LUID */
static const GUID eth0_guid = { 0x4e6f7661, 0x0001, 0x4e6f, { 0x80, 0x00, 0x65, 0x74, 0x68, 0x30, 0x00, 0x01 } };
#define ETH0_LUID (((ULONG64)6 << 48) | ((ULONG64)1 << 24))
IPHLPAPI DWORD WINAPI ConvertInterfaceNameToLuidW(LPCWSTR name, NET_LUID_ *luid)
{
    if (!name || !luid) return ERROR_INVALID_PARAMETER;
    if (lstrcmpiW(name, L"ethernet_1")) { luid->Value = 0; return ERROR_INVALID_NAME; }
    luid->Value = ETH0_LUID;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceNameToLuidA(LPCSTR name, NET_LUID_ *luid)
{
    if (!name || !luid) return ERROR_INVALID_PARAMETER;
    if (lstrcmpiA(name, "ethernet_1")) { luid->Value = 0; return ERROR_INVALID_NAME; }
    luid->Value = ETH0_LUID;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceLuidToGuid(const NET_LUID_ *luid, GUID *guid)
{
    if (!luid || !guid) return ERROR_INVALID_PARAMETER;
    if (luid->Value != ETH0_LUID) { memset(guid, 0, sizeof(*guid)); return ERROR_FILE_NOT_FOUND; }
    *guid = eth0_guid;
    return NO_ERROR;
}
IPHLPAPI DWORD WINAPI ConvertInterfaceGuidToLuid(const GUID *guid, NET_LUID_ *luid)
{
    if (!luid || !guid) return ERROR_INVALID_PARAMETER;
    const BYTE *a = (const BYTE *)guid, *b = (const BYTE *)&eth0_guid;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) { luid->Value = 0; return ERROR_FILE_NOT_FOUND; }
    luid->Value = ETH0_LUID;
    return NO_ERROR;
}

/* The adapter list for DHCP's calls (IP_INTERFACE_INFO): empty, like
 * GetAdaptersInfo's; the kernel keeps the DHCP lease (dhcpcsvc), so there
 * is no adapter here to release or renew */
IPHLPAPI DWORD WINAPI GetInterfaceInfo(PVOID info, PULONG size) { (void)info; if (!size) return ERROR_INVALID_PARAMETER; return ERROR_NO_DATA_; }
IPHLPAPI DWORD WINAPI IpReleaseAddress(PVOID adapter) { return adapter ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER; }
IPHLPAPI DWORD WINAPI IpRenewAddress(PVOID adapter) { return adapter ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER; }
/* NotifyAddrChange never starts one, so there is no request to cancel */
IPHLPAPI BOOL WINAPI CancelIPChangeNotify(LPOVERLAPPED o) { (void)o; SetLastError(ERROR_NOT_FOUND); return FALSE; }
