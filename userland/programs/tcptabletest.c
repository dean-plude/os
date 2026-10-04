/* tcptabletest.exe — iphlpapi's TCP connection tables: a listener and a
 * connection over 127.0.0.1 (and a listener on ::1) show up in
 * GetExtendedTcpTable with their states and this process as the owner,
 * the way a local server (GOG Galaxy's client service) finds which
 * process is calling it; the listener / connection classes, sorting,
 * the size query, GetTcpTable and GetTcp6Table. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#define MIB_TCP_STATE_LISTEN 2
#define MIB_TCP_STATE_ESTAB  5
enum { BASIC_LISTENER, BASIC_CONNECTIONS, BASIC_ALL, PID_LISTENER, PID_CONNECTIONS, PID_ALL,
       MODULE_LISTENER, MODULE_CONNECTIONS, MODULE_ALL };

typedef struct { DWORD state, laddr, lport, raddr, rport; } Row;
typedef struct { DWORD state, laddr, lport, raddr, rport, pid; } PidRow;
typedef struct { BYTE laddr[16]; DWORD lscope, lport; BYTE raddr[16]; DWORD rscope, rport, state, pid; } Pid6Row;
typedef struct { DWORD state; BYTE laddr[16]; DWORD lscope, lport; BYTE raddr[16]; DWORD rscope, rport; } Row6;
typedef struct { DWORD state, laddr, lport, raddr, rport, pid; LARGE_INTEGER created; ULONGLONG module[16]; } ModRow;

typedef DWORD (WINAPI *GetExtendedTcpTable_t)(PVOID, PDWORD, BOOL, ULONG, int, ULONG);
typedef DWORD (WINAPI *GetTcpTable_t)(PVOID, PULONG, BOOL);
static GetExtendedTcpTable_t p_GetExtendedTcpTable;
static GetTcpTable_t p_GetTcpTable, p_GetTcp6Table;

static int passed, failed;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

static BYTE buf[256 * 1024];

static DWORD table(int af, int cls, BOOL order)
{
    DWORD size = sizeof(buf);
    return p_GetExtendedTcpTable(buf, &size, order, af, cls, 0);
}

static USHORT port_of(SOCKET s)
{
    struct sockaddr_in6 a;
    int n = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &n);
    return a.sin6_port;                                     /* (the same offset as sockaddr_in's) */
}

int main(void)
{
    HMODULE ip = LoadLibraryA("iphlpapi.dll");
    p_GetExtendedTcpTable = (GetExtendedTcpTable_t)GetProcAddress(ip, "GetExtendedTcpTable");
    p_GetTcpTable = (GetTcpTable_t)GetProcAddress(ip, "GetTcpTable");
    p_GetTcp6Table = (GetTcpTable_t)GetProcAddress(ip, "GetTcp6Table");
    check("iphlpapi exports the TCP tables", p_GetExtendedTcpTable && p_GetTcpTable && p_GetTcp6Table);
    if (!p_GetExtendedTcpTable || !p_GetTcpTable || !p_GetTcp6Table) { printf("tcptabletest: %d passed, %d failed\n", passed, failed + 1); return 1; }

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    DWORD me = GetCurrentProcessId();
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    int ok = bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(ls, 4) == 0;
    sa.sin_port = port_of(ls);
    SOCKET cs = socket(AF_INET, SOCK_STREAM, 0);
    ok = ok && connect(cs, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    SOCKET as = accept(ls, 0, 0);
    check("a listener and a connection over 127.0.0.1", ok && as != INVALID_SOCKET);
    USHORT lport = sa.sin_port, cport = port_of(cs);

    DWORD size = 0;
    check("a size query gives ERROR_INSUFFICIENT_BUFFER and the size",
          p_GetExtendedTcpTable(0, &size, FALSE, AF_INET, PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && size >= 4 + 3 * sizeof(PidRow));
    check("reserved must be 0", p_GetExtendedTcpTable(buf, &size, FALSE, AF_INET, PID_ALL, 1) == ERROR_INVALID_PARAMETER);
    check("an unknown family is refused", p_GetExtendedTcpTable(buf, &size, FALSE, 99, PID_ALL, 0) == ERROR_INVALID_PARAMETER);

    /* TCP_TABLE_OWNER_PID_ALL: the listener, both ends of the connection, each ours */
    int listen_ok = 0, client_ok = 0, server_ok = 0;
    check("GetExtendedTcpTable(AF_INET, TCP_TABLE_OWNER_PID_ALL)", table(AF_INET, PID_ALL, TRUE) == NO_ERROR);
    DWORD n = *(DWORD *)buf;
    PidRow *pr = (PidRow *)(buf + 4);
    for (DWORD i = 0; i < n; i++) {
        PidRow *r = &pr[i];
        if (r->state == MIB_TCP_STATE_LISTEN && r->lport == lport && r->laddr == htonl(INADDR_LOOPBACK) && !r->raddr && !r->rport && r->pid == me) listen_ok = 1;
        if (r->state == MIB_TCP_STATE_ESTAB && r->lport == cport && r->rport == lport && r->raddr == htonl(INADDR_LOOPBACK) && r->pid == me) client_ok = 1;
        if (r->state == MIB_TCP_STATE_ESTAB && r->lport == lport && r->rport == cport && r->pid == me) server_ok = 1;
    }
    check("... lists the listener (LISTEN, owned here)", listen_ok);
    check("... the client end (ESTABLISHED, owned here)", client_ok);
    check("... and the accepted end (owned here)", server_ok);
    int sorted = 1;
    for (DWORD i = 1; i < n; i++) {
        PidRow *a = &pr[i - 1], *b = &pr[i];
        if (ntohl(a->laddr) > ntohl(b->laddr) || (a->laddr == b->laddr && ntohs((USHORT)a->lport) > ntohs((USHORT)b->lport))) sorted = 0;
    }
    check("... sorted by local address and port", sorted);

    table(AF_INET, PID_LISTENER, FALSE);
    n = *(DWORD *)buf;
    int only_listen = n > 0;
    for (DWORD i = 0; i < n; i++) if (pr[i].state != MIB_TCP_STATE_LISTEN) only_listen = 0;
    check("TCP_TABLE_OWNER_PID_LISTENER has only listeners", only_listen);
    table(AF_INET, PID_CONNECTIONS, FALSE);
    n = *(DWORD *)buf;
    int no_listen = n >= 2;
    for (DWORD i = 0; i < n; i++) if (pr[i].state == MIB_TCP_STATE_LISTEN) no_listen = 0;
    check("TCP_TABLE_OWNER_PID_CONNECTIONS has no listener", no_listen);

    table(AF_INET, MODULE_ALL, FALSE);
    n = *(DWORD *)buf;
    ModRow *mr = (ModRow *)(buf + 8);
    int mod_ok = 0;
    for (DWORD i = 0; i < n; i++) if (mr[i].lport == cport && mr[i].pid == me) mod_ok = 1;
    check("TCP_TABLE_OWNER_MODULE_ALL rows (160 bytes, after 8)", sizeof(ModRow) == 160 && mod_ok);

    ULONG sz = sizeof(buf);
    check("GetTcpTable", p_GetTcpTable(buf, &sz, TRUE) == NO_ERROR);
    n = *(DWORD *)buf;
    Row *br = (Row *)(buf + 4);
    int basic_ok = 0;
    for (DWORD i = 0; i < n; i++) if (br[i].lport == cport && br[i].rport == lport && br[i].state == MIB_TCP_STATE_ESTAB) basic_ok = 1;
    check("... has the connection", basic_ok);

    /* IPv6: a listener on ::1 */
    SOCKET l6 = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 s6 = { 0 };
    s6.sin6_family = AF_INET6;
    s6.sin6_addr.s6_addr[15] = 1;
    int ok6 = l6 != INVALID_SOCKET && bind(l6, (struct sockaddr *)&s6, sizeof(s6)) == 0 && listen(l6, 1) == 0;
    USHORT p6 = port_of(l6);
    int v6_ok = 0, v6_not_v4 = 1;
    if (ok6 && table(AF_INET6, PID_ALL, FALSE) == NO_ERROR) {
        n = *(DWORD *)buf;
        Pid6Row *r6 = (Pid6Row *)(buf + 4);
        for (DWORD i = 0; i < n; i++)
            if (r6[i].lport == p6 && r6[i].laddr[15] == 1 && r6[i].state == MIB_TCP_STATE_LISTEN && r6[i].pid == me) v6_ok = 1;
    }
    check("GetExtendedTcpTable(AF_INET6) lists the ::1 listener", v6_ok);
    table(AF_INET, PID_ALL, FALSE);
    n = *(DWORD *)buf;
    for (DWORD i = 0; i < n; i++) if (pr[i].lport == p6) v6_not_v4 = 0;
    check("... and the IPv4 table does not", v6_not_v4);
    sz = sizeof(buf);
    int t6 = 0;
    if (p_GetTcp6Table(buf, &sz, FALSE) == NO_ERROR) {
        n = *(DWORD *)buf;
        Row6 *r6 = (Row6 *)(buf + 4);
        for (DWORD i = 0; i < n; i++) if (r6[i].lport == p6 && r6[i].state == MIB_TCP_STATE_LISTEN) t6 = 1;
    }
    check("GetTcp6Table lists it too", t6);

    closesocket(cs); closesocket(as); closesocket(ls); closesocket(l6);
    Sleep(200);
    table(AF_INET, PID_LISTENER, FALSE);
    n = *(DWORD *)buf;
    int gone = 1;
    for (DWORD i = 0; i < n; i++) if (pr[i].lport == lport) gone = 0;
    check("a closed listener leaves the table", gone);

    WSACleanup();
    printf("tcptabletest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
