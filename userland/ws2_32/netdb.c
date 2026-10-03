/*
 * netdb.c — the services database (getservbyname/getservbyport: the
 * well-known ports of C:\Windows\System32\drivers\etc\services),
 * gethostbyaddr (the address as its own name: NovaOS does no reverse
 * lookups, as getnameinfo) and WSAPoll (on select).
 */
#define WS2_EXPORT
#define NOVA_BUILD_KERNEL32
#include <winsock2.h>
#include <winternl.h>

static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static void set_err(int e) { *(DWORD *)(NtCurrentTebBytes() + TEB_LAST_ERROR) = (DWORD)e; }


static const struct { const char *name; unsigned short port; unsigned char tcp, udp; } g_services[] = {
    { "echo", 7, 1, 1 },        { "discard", 9, 1, 1 },     { "daytime", 13, 1, 1 },  { "ftp-data", 20, 1, 0 },
    { "ftp", 21, 1, 0 },        { "ssh", 22, 1, 0 },        { "telnet", 23, 1, 0 },   { "smtp", 25, 1, 0 },
    { "time", 37, 1, 1 },       { "domain", 53, 1, 1 },     { "bootps", 67, 0, 1 },   { "bootpc", 68, 0, 1 },
    { "tftp", 69, 0, 1 },       { "gopher", 70, 1, 0 },     { "finger", 79, 1, 0 },   { "http", 80, 1, 0 },
    { "kerberos", 88, 1, 1 },   { "pop3", 110, 1, 0 },      { "sunrpc", 111, 1, 1 },  { "nntp", 119, 1, 0 },
    { "ntp", 123, 0, 1 },       { "netbios-ns", 137, 1, 1 },{ "netbios-ssn", 139, 1, 0 }, { "imap", 143, 1, 0 },
    { "snmp", 161, 0, 1 },      { "ldap", 389, 1, 1 },      { "https", 443, 1, 1 },   { "microsoft-ds", 445, 1, 1 },
    { "syslog", 514, 0, 1 },    { "rtsp", 554, 1, 1 },      { "submission", 587, 1, 0 }, { "ldaps", 636, 1, 0 },
    { "rsync", 873, 1, 0 },     { "ftps", 990, 1, 0 },      { "imaps", 993, 1, 0 },   { "pop3s", 995, 1, 0 },
    { "socks", 1080, 1, 0 },    { "ms-sql-s", 1433, 1, 0 }, { "mqtt", 1883, 1, 0 },   { "mysql", 3306, 1, 0 },
    { "ms-wbt-server", 3389, 1, 0 }, { "sip", 5060, 1, 1 }, { "postgresql", 5432, 1, 0 }, { "vnc", 5900, 1, 0 },
    { "http-alt", 8080, 1, 0 },
};

static struct servent *serv(int i, const char *proto)
{
    static __declspec(thread) struct servent se;
    static __declspec(thread) char *aliases[1];
    static __declspec(thread) char name[32], pr[4];
    const char *p = proto ? proto : g_services[i].tcp ? "tcp" : "udp";
    int k = 0;
    for (; g_services[i].name[k] && k < 31; k++) name[k] = g_services[i].name[k];
    name[k] = 0;
    for (k = 0; p[k] && k < 3; k++) pr[k] = p[k];
    pr[k] = 0;
    aliases[0] = 0;
    se.s_name = name;
    se.s_aliases = aliases;
    se.s_port = (short)htons(g_services[i].port);
    se.s_proto = pr;
    return &se;
}

static int proto_ok(int i, const char *proto)
{
    if (!proto) return 1;
    if (ieq(proto, "tcp")) return g_services[i].tcp;
    if (ieq(proto, "udp")) return g_services[i].udp;
    return 0;
}

struct servent *getservbyname(const char *name, const char *proto)
{
    if (!name) { set_err(WSAEFAULT); return 0; }
    for (int i = 0; i < (int)(sizeof(g_services) / sizeof(g_services[0])); i++)
        if (ieq(name, g_services[i].name) && proto_ok(i, proto)) return serv(i, proto);
    set_err(WSANO_DATA);
    return 0;
}

__declspec(dllexport) struct servent *WSAAPI getservbyport(int port, const char *proto)
{
    unsigned short p = ntohs((unsigned short)port);
    for (int i = 0; i < (int)(sizeof(g_services) / sizeof(g_services[0])); i++)
        if (g_services[i].port == p && proto_ok(i, proto)) return serv(i, proto);
    set_err(WSANO_DATA);
    return 0;
}

__declspec(dllexport) struct hostent *WSAAPI gethostbyaddr(const char *addr, int len, int type)
{
    static __declspec(thread) struct hostent he;
    static __declspec(thread) ULONG a;
    static __declspec(thread) char *alist[2], *aliases[1];
    static __declspec(thread) char name[16];
    if (!addr) { set_err(WSAEFAULT); return 0; }
    if (type != AF_INET || len < 4) { set_err(WSAEAFNOSUPPORT); return 0; }
    const unsigned char *b = (const unsigned char *)addr;
    int k = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) name[k++] = (char)('0' + v / 100);
        if (v >= 10) name[k++] = (char)('0' + v / 10 % 10);
        name[k++] = (char)('0' + v % 10);
        if (i < 3) name[k++] = '.';
    }
    name[k] = 0;
    a = *(const ULONG *)addr;
    alist[0] = (char *)&a; alist[1] = 0;
    aliases[0] = 0;
    he.h_name = name;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = alist;
    return &he;
}

/* WSAPoll: select underneath */
typedef struct { SOCKET fd; short events, revents; } WSAPOLLFD_;
#define POLLRDNORM_ 0x0100
#define POLLRDBAND_ 0x0200
#define POLLPRI_    0x0400
#define POLLWRNORM_ 0x0010
#define POLLWRBAND_ 0x0020
#define POLLERR_    0x0001
#define POLLNVAL_   0x0004

__declspec(dllexport) int WSAAPI WSAPoll(WSAPOLLFD_ *fds, ULONG n, INT timeout)
{
    if (!fds && n) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    if (n > FD_SETSIZE) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    int any = 0;
    for (ULONG i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd == INVALID_SOCKET) continue;
        if (fds[i].events & (POLLRDNORM_ | POLLRDBAND_)) FD_SET(fds[i].fd, &rd);
        if (fds[i].events & (POLLWRNORM_ | POLLWRBAND_)) FD_SET(fds[i].fd, &wr);
        FD_SET(fds[i].fd, &ex);
        any = 1;
    }
    if (!any) { set_err(WSAEINVAL); return SOCKET_ERROR; }
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
    int r = select(0, &rd, &wr, &ex, timeout < 0 ? 0 : &tv);
    if (r == SOCKET_ERROR) {
        /* a bad socket among them: report it (POLLNVAL) rather than fail */
        int bad = 0;
        for (ULONG i = 0; i < n; i++) {
            if (fds[i].fd == INVALID_SOCKET) continue;
            fd_set one;
            struct timeval z = { 0, 0 };
            FD_ZERO(&one); FD_SET(fds[i].fd, &one);
            if (select(0, 0, 0, &one, &z) == SOCKET_ERROR) { fds[i].revents = POLLNVAL_; bad++; }
        }
        return bad ? bad : SOCKET_ERROR;
    }
    int ready = 0;
    for (ULONG i = 0; i < n; i++) {
        SOCKET s = fds[i].fd;
        if (s == INVALID_SOCKET) continue;
        short ev = 0;
        /* a closed peer reads as readable: the next recv returns 0 */
        if (__WSAFDIsSet(s, &rd)) ev |= fds[i].events & (POLLRDNORM_ | POLLRDBAND_);
        if (__WSAFDIsSet(s, &wr)) ev |= fds[i].events & (POLLWRNORM_ | POLLWRBAND_);
        if (__WSAFDIsSet(s, &ex)) ev |= POLLERR_;
        fds[i].revents = ev;
        if (ev) ready++;
    }
    return ready;
}
